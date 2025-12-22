#include "QuadTree.hpp"

QuadTree::QuadTree(const sf::FloatRect& rect, const int depth)
    : m_boundary{rect}
    , m_depth{depth}
    , m_divided{false}
{}

bool QuadTree::insert(entt::registry& reg, const entt::entity entityID)
{
    // Ignore objects that do not belong in this quad tree
    if (!m_boundary.contains(reg.get<Sprite>(entityID).getPosition()))
        return false;

    // Leaf fast-path: no children yet, keep entities locally until we must subdivide.
    if (!m_divided)
    {
        if (m_nodes.size() < QT_NODE_CAPACITY || (m_depth + 1 >= MAX_DEPTH))
        {
            m_nodes.push_back(entityID);
            return true;
        }

        subdivideAndRehome(reg);
    }

    // Child path: try to push into a subtree.
    if (insertIntoChildren(reg, entityID))
        return true;

    // Fallback: keep in this node if children reject (should be rare with point containment).
    m_nodes.push_back(entityID);
    return true;
}

bool QuadTree::insertIntoChildren(entt::registry& reg, const entt::entity entityID)
{
    QuadTree* const children[] = {
        m_northWest.get(),
        m_northEast.get(),
        m_southEast.get(),
        m_southWest.get(),
    };

    for (auto* child : children)
    {
        if (child && child->insert(reg, entityID))
            return true;
    }

    return false;
}

void QuadTree::subdivideAndRehome(entt::registry& reg)
{
    // Only subdivide when allowed.
    if (m_divided || (m_depth + 1 >= MAX_DEPTH))
        return;

    subdivide();

    // Re-home existing nodes into children so the tree actually reduces query cost.
    auto existing = std::move(m_nodes);
    m_nodes.clear();

    for (const auto e : existing)
    {
        if (!insertIntoChildren(reg, e))
        {
            // Fallback: keep in this node if children reject.
            m_nodes.push_back(e);
        }
    }
}

bool QuadTree::hasAny() const
{
    if (!m_nodes.empty()) return true;
    if (!m_divided) return false;

    return (m_northWest && m_northWest->hasAny()) ||
           (m_northEast && m_northEast->hasAny()) ||
           (m_southEast && m_southEast->hasAny()) ||
           (m_southWest && m_southWest->hasAny());
}

void QuadTree::subdivide()
{
    const float left = m_boundary.position.x;
    const float top = m_boundary.position.y;
    const float width = m_boundary.size.x;
    const float height = m_boundary.size.y;

    sf::Rect<float> nwRect({left, top}, {(width / 2.f), (height / 2.f)});
    sf::Rect<float> neRect({(left + (width / 2.f)), top}, {(width / 2.f), (height / 2.f)});
    sf::Rect<float> seRect({(left + (width / 2.f)), (top + (height / 2.f))}, {(width / 2.f), (height / 2.f)});
    sf::Rect<float> swRect({left, (top + (height / 2.f))}, {(width / 2.f), (height / 2.f)});

    m_northWest = std::make_unique<QuadTree>(nwRect, m_depth + 1);
    m_northEast = std::make_unique<QuadTree>(neRect, m_depth + 1);
    m_southEast = std::make_unique<QuadTree>(seRect, m_depth + 1);
    m_southWest = std::make_unique<QuadTree>(swRect, m_depth + 1);

    m_divided = true;
}

std::vector<entt::entity> QuadTree::queryRange(entt::registry& reg, const sf::FloatRect& range)
{
    std::vector<entt::entity> entityFound;
    entityFound.reserve(16);
    queryRangeImpl(reg, range, entityFound);
    return entityFound;
}

void QuadTree::queryRangeImpl(entt::registry& reg, const sf::FloatRect& range, std::vector<entt::entity>& out)
{
    if (!m_boundary.findIntersection(range).has_value())
        return;

    for (const auto entity : m_nodes)
    {
        if (reg.valid(entity) &&
            reg.all_of<Sprite>(entity) &&
            range.findIntersection(reg.get<Sprite>(entity).getGlobalBounds()).has_value())
        {
            out.push_back(entity);
        }
    }

    if (!m_divided)
        return;

    m_northWest->queryRangeImpl(reg, range, out);
    m_northEast->queryRangeImpl(reg, range, out);
    m_southEast->queryRangeImpl(reg, range, out);
    m_southWest->queryRangeImpl(reg, range, out);
}

bool QuadTree::remove(entt::registry& reg, const entt::entity entityID)
{
    for (size_t i = 0; i < m_nodes.size(); ++i)
    {
        if (m_nodes[i] == entityID)
        {
            // Swap-erase to avoid shifting
            m_nodes[i] = m_nodes.back();
            m_nodes.pop_back();
            return true;
        }
    }

    if (!m_divided) return false;

    if (m_northWest->remove(reg, entityID)) return true;
    if (m_northEast->remove(reg, entityID)) return true;
    if (m_southEast->remove(reg, entityID)) return true;
    if (m_southWest->remove(reg, entityID)) return true;

    return false;
}

void QuadTree::clear()
{
    m_nodes.clear();

    // Collapse the tree so the visualizer reflects the current active structure.
    // (Children will be recreated on-demand by insert().)
    if (m_divided)
    {
        m_northWest.reset();
        m_northEast.reset();
        m_southEast.reset();
        m_southWest.reset();
        m_divided = false;
    }
}

void QuadTree::draw(sf::RenderTexture& rt)
{
    // Only draw "active" subtrees (except root which is always drawn).
    if (m_depth != 0 && !hasAny())
        return;

    sf::RectangleShape border;
    border.setPosition({m_boundary.position.x, m_boundary.position.y});
    border.setSize({m_boundary.size.x, m_boundary.size.y});
    border.setOutlineThickness(1.0f);
    border.setFillColor(sf::Color::Transparent);
    border.setOutlineColor(sf::Color(0, 150, 100));

    rt.draw(border);

    if (m_divided)
    {
        // Draw the split lines for this node regardless of which children are "active".
        // Otherwise, if only one quadrant has entities, drawing just that child produces
        // an L-shape that looks like an incomplete subdivision.
        const float midX = m_boundary.position.x + (m_boundary.size.x / 2.f);
        const float midY = m_boundary.position.y + (m_boundary.size.y / 2.f);

        sf::RectangleShape vLine;
        vLine.setPosition({ midX, m_boundary.position.y });
        vLine.setSize({ 1.0f, m_boundary.size.y });
        vLine.setFillColor(sf::Color(0, 150, 100));

        sf::RectangleShape hLine;
        hLine.setPosition({ m_boundary.position.x, midY });
        hLine.setSize({ m_boundary.size.x, 1.0f });
        hLine.setFillColor(sf::Color(0, 150, 100));

        rt.draw(vLine);
        rt.draw(hLine);

        m_northWest->draw(rt);
        m_northEast->draw(rt);
        m_southEast->draw(rt);
        m_southWest->draw(rt);
    }
}