#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <SFML/Graphics/BlendMode.hpp>
#include <SFML/Graphics/PrimitiveType.hpp>
#include <SFML/Graphics/RenderStates.hpp>
#include <SFML/Graphics/RenderTarget.hpp>
#include <SFML/Graphics/Shader.hpp>
#include <SFML/Graphics/Texture.hpp>
#include <SFML/Graphics/Transform.hpp>
#include <SFML/Graphics/Vertex.hpp>
#include <SFML/Graphics/VertexArray.hpp>
#include <SFML/Graphics/VertexBuffer.hpp>
#include <SFML/System/Angle.hpp>

#include "RenderCommands2D.hpp"

/**
 * Uniform 2D batching API with two backends:
 * - VertexArray backend: CPU-side geometry, drawn via sf::VertexArray
 * - VertexBuffer backend: uploads per-batch to a GPU vertex buffer, drawn via sf::VertexBuffer
 *
 * This is designed to be:
 * - easy to use (submit -> flush)
 * - safe by default (preserves submission order unless explicitly told to reorder)
 * - extensible (material key includes texture, blend mode, shader)
 */

enum class Batch2DBackend
{
    VertexArray,
    VertexBuffer
};

enum class Batch2DSortMode
{
    PreserveOrder,
    ByMaterial // may change draw order
};

struct Batch2DMaterial
{
    const sf::Texture* texture = nullptr;
    const sf::Shader* shader = nullptr;
    sf::BlendMode blendMode = sf::BlendAlpha;

    [[nodiscard]] bool operator==(const Batch2DMaterial& other) const
    {
        return texture == other.texture && shader == other.shader && blendMode == other.blendMode;
    }

    [[nodiscard]] bool operator!=(const Batch2DMaterial& other) const { return !(*this == other); }

    [[nodiscard]] static bool less(const Batch2DMaterial& a, const Batch2DMaterial& b)
    {
        const auto blendTuple = [](const sf::BlendMode& bm)
        {
            return std::tuple{
                bm.colorSrcFactor,
                bm.colorDstFactor,
                bm.colorEquation,
                bm.alphaSrcFactor,
                bm.alphaDstFactor,
                bm.alphaEquation
            };
        };

        return std::tuple{ a.texture, a.shader, blendTuple(a.blendMode) } <
               std::tuple{ b.texture, b.shader, blendTuple(b.blendMode) };
    }
};

struct Batch2DStats
{
    std::size_t submittedQuads = 0;
    std::size_t submittedVertices = 0;
    std::size_t drawCalls = 0;
};

struct Batch2DConfig
{
    Batch2DBackend backend = Batch2DBackend::VertexArray;
    Batch2DSortMode sortMode = Batch2DSortMode::PreserveOrder;

    // Safety valve: flush when a batch would exceed this many vertices.
    // Must be a multiple of 3 (triangles).
    std::size_t maxVerticesPerDraw = 60000;

    // Only relevant if backend == VertexBuffer.
    sf::VertexBuffer::Usage vertexBufferUsage = sf::VertexBuffer::Usage::Dynamic;
};

namespace detail
{
inline sf::Transform buildTransform(const SpriteDrawCmd& cmd)
{
    sf::Transform t;
    t.translate(cmd.position);
    t.rotate(sf::degrees(cmd.rotationDeg));
    t.scale(cmd.scale);
    t.translate(-cmd.origin);
    return t;
}

inline void appendQuadAsTriangles(
    std::vector<sf::Vertex>& out,
    const sf::Vector2f& p0,
    const sf::Vector2f& p1,
    const sf::Vector2f& p2,
    const sf::Vector2f& p3,
    const sf::Vector2f& t0,
    const sf::Vector2f& t1,
    const sf::Vector2f& t2,
    const sf::Vector2f& t3,
    const sf::Color& color)
{
    // Triangle 1: 0,1,2
    out.emplace_back(p0, color, t0);
    out.emplace_back(p1, color, t1);
    out.emplace_back(p2, color, t2);

    // Triangle 2: 0,2,3
    out.emplace_back(p0, color, t0);
    out.emplace_back(p2, color, t2);
    out.emplace_back(p3, color, t3);
}
} // namespace detail

class IBatchBackend2D
{
public:
    virtual ~IBatchBackend2D() = default;
    virtual void draw(sf::RenderTarget& target, const sf::Vertex* vertices, std::size_t count, const sf::RenderStates& states) = 0;
};

class VertexArrayBatchBackend2D final : public IBatchBackend2D
{
public:
    VertexArrayBatchBackend2D()
        : m_va(sf::PrimitiveType::Triangles)
    {
    }

    void draw(sf::RenderTarget& target, const sf::Vertex* vertices, std::size_t count, const sf::RenderStates& states) override
    {
        m_va.clear();
        m_va.resize(count);
        for (std::size_t i = 0; i < count; ++i)
            m_va[i] = vertices[i];

        target.draw(m_va, states);
    }

private:
    sf::VertexArray m_va;
};

class VertexBufferBatchBackend2D final : public IBatchBackend2D
{
public:
    explicit VertexBufferBatchBackend2D(sf::VertexBuffer::Usage usage)
        : m_vb(sf::PrimitiveType::Triangles, usage)
    {
    }

    void draw(sf::RenderTarget& target, const sf::Vertex* vertices, std::size_t count, const sf::RenderStates& states) override
    {
        if (count == 0) return;

        if (m_vb.getVertexCount() < count)
            (void)m_vb.create(count);

        // Upload this batch and draw only the used range.
        (void)m_vb.update(vertices, count, 0);
        target.draw(m_vb, 0, count, states);
    }

    [[nodiscard]] static bool isSupported()
    {
        return sf::VertexBuffer::isAvailable();
    }

private:
    sf::VertexBuffer m_vb;
};

class BatchRenderer2D
{
public:
    explicit BatchRenderer2D(Batch2DConfig cfg = {})
        : m_cfg(std::move(cfg))
    {
        rebuildBackend();
    }

    void setBackend(Batch2DBackend backend)
    {
        if (m_cfg.backend == backend) return;
        m_cfg.backend = backend;
        rebuildBackend();
    }

    [[nodiscard]] Batch2DBackend backend() const { return m_cfg.backend; }

    void setSortMode(Batch2DSortMode sortMode) { m_cfg.sortMode = sortMode; }
    [[nodiscard]] Batch2DSortMode sortMode() const { return m_cfg.sortMode; }

    void setMaxVerticesPerDraw(std::size_t maxVerts)
    {
        // Keep it triangle-aligned.
        if (maxVerts < 3) maxVerts = 3;
        maxVerts -= (maxVerts % 3);
        m_cfg.maxVerticesPerDraw = maxVerts;
    }

    void begin(const sf::RenderStates& baseStates = sf::RenderStates::Default)
    {
        m_baseStates = baseStates;
        clear();
    }

    void clear()
    {
        m_items.clear();
        m_stats = {};
    }

    [[nodiscard]] const Batch2DStats& stats() const { return m_stats; }

    // --- Submission API (uniform) ---

    void submit(const SpriteDrawCmd& cmd, const Batch2DMaterial* overrideMaterial = nullptr)
    {
        if (!cmd.texture) return;

        Batch2DMaterial mat;
        if (overrideMaterial)
            mat = *overrideMaterial;
        else
            mat.texture = cmd.texture;

        // Inherit base state unless explicitly overridden.
        if (!overrideMaterial || overrideMaterial->shader == nullptr)
            mat.shader = m_baseStates.shader;
        if (!overrideMaterial)
            mat.blendMode = m_baseStates.blendMode;

        const auto texLeft = static_cast<float>(cmd.texRect.position.x);
        const auto texTop = static_cast<float>(cmd.texRect.position.y);
        const auto texRight = texLeft + static_cast<float>(cmd.texRect.size.x);
        const auto texBottom = texTop + static_cast<float>(cmd.texRect.size.y);

        const float absW = std::abs(static_cast<float>(cmd.texRect.size.x));
        const float absH = std::abs(static_cast<float>(cmd.texRect.size.y));

        const sf::Transform xform = detail::buildTransform(cmd);
        const sf::Vector2f p0 = xform.transformPoint({ 0.f, 0.f });
        const sf::Vector2f p1 = xform.transformPoint({ absW, 0.f });
        const sf::Vector2f p2 = xform.transformPoint({ absW, absH });
        const sf::Vector2f p3 = xform.transformPoint({ 0.f, absH });

        const sf::Vector2f t0{ texLeft, texTop };
        const sf::Vector2f t1{ texRight, texTop };
        const sf::Vector2f t2{ texRight, texBottom };
        const sf::Vector2f t3{ texLeft, texBottom };

        Item it;
        it.material = mat;
        it.color = cmd.color;
        it.pos = { p0, p1, p2, p3 };
        it.tex = { t0, t1, t2, t3 };
        m_items.push_back(it);

        ++m_stats.submittedQuads;
        m_stats.submittedVertices += 6;
    }

    void submit(const RectDrawCmd& cmd, const Batch2DMaterial* overrideMaterial = nullptr)
    {
        Batch2DMaterial mat;
        if (overrideMaterial)
            mat = *overrideMaterial;

        // Rects are untextured by default; inherit base state unless explicitly overridden.
        if (!overrideMaterial || overrideMaterial->shader == nullptr)
            mat.shader = m_baseStates.shader;
        if (!overrideMaterial)
            mat.blendMode = m_baseStates.blendMode;

        const sf::Vector2f p0 = cmd.position;
        const sf::Vector2f p1{ cmd.position.x + cmd.size.x, cmd.position.y };
        const sf::Vector2f p2{ cmd.position.x + cmd.size.x, cmd.position.y + cmd.size.y };
        const sf::Vector2f p3{ cmd.position.x, cmd.position.y + cmd.size.y };

        const sf::Vector2f t0{ 0.f, 0.f };
        const sf::Vector2f t1{ 0.f, 0.f };
        const sf::Vector2f t2{ 0.f, 0.f };
        const sf::Vector2f t3{ 0.f, 0.f };

        Item it;
        it.material = mat;
        it.color = cmd.color;
        it.pos = { p0, p1, p2, p3 };
        it.tex = { t0, t1, t2, t3 };
        m_items.push_back(it);

        ++m_stats.submittedQuads;
        m_stats.submittedVertices += 6;
    }

    void submit(const RenderCommands2D& cmds)
    {
        for (const auto& s : cmds.sprites) submit(s);
        for (const auto& r : cmds.rects) submit(r);
    }

    // Flushes everything submitted since begin()/clear().
    // Returns stats from this flush, and keeps them available via stats().
    Batch2DStats flush(sf::RenderTarget& target)
    {
        if (m_items.empty())
            return m_stats;

        if (m_cfg.sortMode == Batch2DSortMode::ByMaterial)
        {
            std::stable_sort(
                m_items.begin(),
                m_items.end(),
                [](const Item& a, const Item& b) { return Batch2DMaterial::less(a.material, b.material); }
            );
        }

        m_vertices.clear();
        m_vertices.reserve(std::min<std::size_t>(m_items.size() * 6, m_cfg.maxVerticesPerDraw));

        Batch2DMaterial current = m_items.front().material;
        for (const auto& item : m_items)
        {
            const bool materialChanged = (item.material != current);
            const bool wouldOverflow = (m_vertices.size() + 6 > m_cfg.maxVerticesPerDraw);

            if ((materialChanged || wouldOverflow) && !m_vertices.empty())
            {
                drawCurrent(target, current);
                current = item.material;
            }

            detail::appendQuadAsTriangles(
                m_vertices,
                item.pos[0],
                item.pos[1],
                item.pos[2],
                item.pos[3],
                item.tex[0],
                item.tex[1],
                item.tex[2],
                item.tex[3],
                item.color
            );
        }

        if (!m_vertices.empty())
            drawCurrent(target, current);

        return m_stats;
    }

private:
    struct Item
    {
        Batch2DMaterial material;
        sf::Color color{};
        std::array<sf::Vector2f, 4> pos{};
        std::array<sf::Vector2f, 4> tex{};
    };

private:
    void rebuildBackend()
    {
        if (m_cfg.backend == Batch2DBackend::VertexBuffer)
        {
            if (VertexBufferBatchBackend2D::isSupported())
            {
                m_backend = std::make_unique<VertexBufferBatchBackend2D>(m_cfg.vertexBufferUsage);
                return;
            }

            // Fallback: vertex buffers not supported.
            m_cfg.backend = Batch2DBackend::VertexArray;
        }

        m_backend = std::make_unique<VertexArrayBatchBackend2D>();
    }

    void drawCurrent(sf::RenderTarget& target, const Batch2DMaterial& mat)
    {
        sf::RenderStates states = m_baseStates;
        states.texture = mat.texture;
        states.shader = mat.shader;
        states.blendMode = mat.blendMode;

        m_backend->draw(target, m_vertices.data(), m_vertices.size(), states);
        ++m_stats.drawCalls;
        m_vertices.clear();
    }

private:
    Batch2DConfig m_cfg;
    sf::RenderStates m_baseStates = sf::RenderStates::Default;

    std::unique_ptr<IBatchBackend2D> m_backend;

    std::vector<Item> m_items;
    std::vector<sf::Vertex> m_vertices; // scratch for the currently-building batch
    Batch2DStats m_stats{};
};

