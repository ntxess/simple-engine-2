#include "SemanticProjection.hpp"

#include <algorithm>
#include <cinttypes>
#include <utility>
#include <vector>

#include "component/EntityStatus.hpp"
#include "component/SpawnIndex.hpp"
#include "component/Sprite.hpp"

namespace
{
    // Appends %.9g, which round-trips a float.
    void appendFloat(std::string& out, float value)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(value));
        out += buf;
    }

    void appendStatName(std::string& out, std::string_view name)
    {
        for (const char c : name)
            out += (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '=') ? '_' : c;
    }
}

SemanticProjection::~SemanticProjection()
{
    close();
}

bool SemanticProjection::open(const std::string& path, std::uint32_t rate)
{
    close();
    m_file = std::fopen(path.c_str(), "wb");
    if (!m_file)
        return false;

    std::fprintf(m_file,
        "# seproj v1 rate=%u\n"
        "# t <tick> <fnv1a64 of the block>, then per entity in SpawnIndex order:\n"
        "# <spawn> <x> <y> <rotation> <HP> [<stat>=<value> ...] (%%.9g, '-' = absent)\n",
        rate);
    return true;
}

void SemanticProjection::close()
{
    if (m_file)
    {
        std::fprintf(m_file, "# run %" PRIu64 " %016" PRIx64 "\n", m_ticksWritten, m_runHash);
        std::fclose(m_file);
        m_file = nullptr;
    }
}

void SemanticProjection::write(std::uint64_t tick, const entt::registry& reg)
{
    m_block.clear();
    formatTick(tick, reg, m_block);

    m_tickHash = fnv1a(fnvOffsetBasis, m_block);
    m_runHash = fnv1a(m_runHash, m_block);
    ++m_ticksWritten;

    if (m_file)
    {
        // The stored tick line carries the hash; the hashed block has the bare "t <tick>" line.
        const std::size_t firstNewline = m_block.find('\n');
        std::fprintf(m_file, "%.*s %016" PRIx64 "\n", static_cast<int>(firstNewline), m_block.data(), m_tickHash);
        std::fwrite(m_block.data() + firstNewline + 1, 1, m_block.size() - firstNewline - 1, m_file);
    }
}

void SemanticProjection::formatTick(std::uint64_t tick, const entt::registry& reg, std::string& out)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "t %" PRIu64 "\n", tick);
    out += buf;

    std::vector<std::pair<std::uint32_t, entt::entity>> entities;
    if (const auto* spawnIndices = reg.storage<SpawnIndex>())
    {
        entities.reserve(spawnIndices->size());
        for (const auto [entity, spawnIndex] : spawnIndices->each())
            entities.emplace_back(spawnIndex.value, entity);
    }
    std::sort(entities.begin(), entities.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::pair<std::string_view, float>> stats;
    for (const auto& [spawnIndex, entity] : entities)
    {
        std::snprintf(buf, sizeof(buf), "%" PRIu32, spawnIndex);
        out += buf;

        if (const auto* sprite = reg.try_get<Sprite>(entity))
        {
            out += ' ';
            appendFloat(out, sprite->getPosition().x);
            out += ' ';
            appendFloat(out, sprite->getPosition().y);
            out += ' ';
            appendFloat(out, sprite->getRotation().asDegrees());
        }
        else
        {
            out += " - - -";
        }

        const auto* status = reg.try_get<EntityStatus>(entity);
        out += ' ';
        if (status && status->has("HP"))
            appendFloat(out, status->getOr("HP"));
        else
            out += '-';

        if (status)
        {
            stats.clear();
            for (const auto& [name, value] : status->values)
            {
                if (name != "HP")
                    stats.emplace_back(name, value);
            }
            std::sort(stats.begin(), stats.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [name, value] : stats)
            {
                out += ' ';
                appendStatName(out, name);
                out += '=';
                appendFloat(out, value);
            }
        }
        out += '\n';
    }
}

std::uint64_t SemanticProjection::fnv1a(std::uint64_t hash, std::string_view bytes)
{
    for (const char c : bytes)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= fnvPrime;
    }
    return hash;
}
