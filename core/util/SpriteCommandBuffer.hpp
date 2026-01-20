#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

#include <SFML/Graphics/Color.hpp>
#include <SFML/Graphics/Rect.hpp>
#include <SFML/System/Vector2.hpp>

namespace sf
{
class Texture;
}

/**
 * Minimal render command representation for 2D sprites.
 *
 * Designed to be produced on the simulation thread and consumed on the render thread
 * without touching the live ECS/registry.
 */
struct SpriteDrawCmd
{
    const sf::Texture* texture = nullptr;
    sf::IntRect texRect{};
    sf::Vector2f position{};
    sf::Vector2f scale{1.f, 1.f};
    sf::Vector2f origin{};
    sf::Color color{255, 255, 255, 255};
    float rotationDeg = 0.f;
};

/**
 * Triple-buffered sprite command list.
 *
 * - Producer thread: beginWrite() -> fill vector -> publish()
 * - Consumer thread: acquireRead() -> draw
 *
 * Triple buffering avoids producer overwriting the buffer currently being drawn.
 */
class TripleBufferedSpriteCommands
{
public:
    std::vector<SpriteDrawCmd>& beginWrite()
    {
        const int readIdx = m_readIndex.load(std::memory_order_acquire);
        if (m_writeIndex == readIdx)
            m_writeIndex = (m_writeIndex + 1) % 3;
        if (m_writeIndex == readIdx)
            m_writeIndex = (m_writeIndex + 1) % 3;
        return m_buffers[static_cast<std::size_t>(m_writeIndex)];
    }

    void publish()
    {
        m_readyIndex.store(m_writeIndex, std::memory_order_release);
        m_writeIndex = (m_writeIndex + 1) % 3;
    }

    const std::vector<SpriteDrawCmd>& acquireRead() const
    {
        const int idx = m_readyIndex.load(std::memory_order_acquire);
        m_readIndex.store(idx, std::memory_order_release);
        return m_buffers[static_cast<std::size_t>(idx)];
    }

private:
    mutable std::atomic<int> m_readIndex{0};
    std::atomic<int> m_readyIndex{0};
    int m_writeIndex = 1;

    mutable std::array<std::vector<SpriteDrawCmd>, 3> m_buffers{};
};

