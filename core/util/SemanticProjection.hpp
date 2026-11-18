#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <entt/entity/registry.hpp>

/**
 * @brief Per-tick semantic projection of a scene: what the gameplay rules decide, independent
 * of entity ids, storage order and rendering (docs/milestone-specs.md, M1a).
 *
 * For every tick it formats one block of canonical text:
 *
 *     t <tick>
 *     <spawn> <x> <y> <rotation> <HP> [<stat>=<value> ...]
 *     ...
 *
 * with one line per entity that has a SpawnIndex, ordered by SpawnIndex: the sprite position and
 * rotation in degrees, the HP stat and the remaining EntityStatus stats sorted by name. Numbers
 * are printed with %.9g (float values round-trip); "-" marks a missing component or stat, and
 * whitespace or '=' in a stat name becomes '_'. Liveness is the presence of the line.
 *
 * The tick hash is FNV-1a 64 over that block. The run hash continues the same FNV-1a state over
 * every block in tick order. A .seproj file stores the blocks with the hash on the tick line
 * ("t <tick> <hash>", 16 hex digits), "#" comment lines, and a final "# run <ticks> <hash>".
*/
class SemanticProjection
{
public:
    static constexpr std::uint64_t fnvOffsetBasis = 0xcbf29ce484222325ULL;
    static constexpr std::uint64_t fnvPrime = 0x00000100000001b3ULL;

    SemanticProjection() = default;
    ~SemanticProjection();
    SemanticProjection(const SemanticProjection&) = delete;
    SemanticProjection& operator=(const SemanticProjection&) = delete;

    // Starts writing a .seproj file; without one only the hashes are computed.
    bool open(const std::string& path, std::uint32_t rate);
    void close();

    void write(std::uint64_t tick, const entt::registry& reg);

    std::uint64_t tickHash() const { return m_tickHash; }
    std::uint64_t runHash() const { return m_runHash; }
    std::uint64_t ticksWritten() const { return m_ticksWritten; }

    // Appends the canonical block of one tick to `out`.
    static void formatTick(std::uint64_t tick, const entt::registry& reg, std::string& out);
    static std::uint64_t fnv1a(std::uint64_t hash, std::string_view bytes);

private:
    std::FILE* m_file = nullptr;
    std::string m_block;
    std::uint64_t m_tickHash = fnvOffsetBasis;
    std::uint64_t m_runHash = fnvOffsetBasis;
    std::uint64_t m_ticksWritten = 0;
};
