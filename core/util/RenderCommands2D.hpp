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
 * 2D render commands intended for a snapshot/command-buffer renderer.
 *
 * Producer (sim thread) builds commands from ECS state; consumer (render thread) draws commands
 * without reading the live registry.
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

struct RectDrawCmd
{
    sf::Vector2f position{};
    sf::Vector2f size{};
    sf::Color color{255, 255, 255, 255};
};

struct RenderCommands2D
{
    std::vector<SpriteDrawCmd> sprites;
    std::vector<RectDrawCmd> rects;

    void clear()
    {
        sprites.clear();
        rects.clear();
    }
};

/**
 * Triple-buffered render command storage.
 *
 * Triple buffering avoids the producer overwriting the buffer currently being consumed.
 */
class TripleBufferedRenderCommands2D
{
public:
    RenderCommands2D& beginWrite()
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

    const RenderCommands2D& acquireRead() const
    {
        const int idx = m_readyIndex.load(std::memory_order_acquire);
        m_readIndex.store(idx, std::memory_order_release);
        return m_buffers[static_cast<std::size_t>(idx)];
    }

private:
    mutable std::atomic<int> m_readIndex{0};
    std::atomic<int> m_readyIndex{0};
    int m_writeIndex = 1;

    mutable std::array<RenderCommands2D, 3> m_buffers{};
};

