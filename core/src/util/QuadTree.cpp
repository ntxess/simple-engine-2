#include "QuadTree.hpp"

QuadTree::QuadTree(const sf::FloatRect& rect, const int depth)
    : m_boundary(rect)
    , m_depth(depth)
    , m_divided(false)
{}

bool QuadTree::insert(entt::registry& reg, const entt::entity entityID)
{
    // Ignore objects that do not belong in this quad tree
    if (!m_boundary.contains(reg.get<Sprite>(entityID).sprite.getPosition()))
        return false;

    // If there is space in this quad tree and if doesn't have subdivisions, add the object here
    if (m_nodes.size() <= QT_NODE_CAPACITY)
    {
        m_nodes.push_back(entityID);
        return true;
    }

    // Otherwise, subdivide and then add the point to whichever node will accept it
    if (m_depth + 1 < MAX_DEPTH)
    {
        if (!m_divided)
            subdivide();

        // We have to add the points/data contained into this quad array to the new quads if we only want
        // the last node to hold the data
        if (m_northWest->insert(reg, entityID)) return true;
        if (m_northEast->insert(reg, entityID)) return true;
        if (m_southEast->insert(reg, entityID)) return true;
        if (m_southWest->insert(reg, entityID)) return true;
    }
    else
    {
        m_nodes.push_back(entityID);
        return true;
    }

    // Otherwise, the point cannot be inserted for some unknown reason (this should never happen)
    return false;
}

void QuadTree::subdivide()
{
    float left = m_boundary.left;
    float top = m_boundary.top;
    float width = m_boundary.width;
    float height = m_boundary.height;

    m_northWest = std::make_unique<QuadTree>(sf::FloatRect(left, top, (width / 2.f), (height / 2.f)), m_depth + 1);
    m_northEast = std::make_unique<QuadTree>(sf::FloatRect((left + (width / 2.f)), top, (width / 2.f), (height / 2.f)), m_depth + 1);
    m_southEast = std::make_unique<QuadTree>(sf::FloatRect((left + (width / 2.f)), (top + (height / 2.f)), (width / 2.f), (height / 2.f)), m_depth + 1);
    m_southWest = std::make_unique<QuadTree>(sf::FloatRect(left, (top + (height / 2.f)), (width / 2.f), (height / 2.f)), m_depth + 1);

    m_divided = true;
}

std::vector<entt::entity> QuadTree::queryRange(entt::registry& reg, const sf::FloatRect& range)
{
    std::vector<entt::entity> entityFound;

    if (!m_boundary.intersects(range))
        return entityFound;

    for (auto entity : m_nodes)
    {
        if (reg.valid(entity) && reg.all_of<Sprite>(entity) && range.intersects(reg.get<Sprite>(entity).getGlobalBounds()))
            entityFound.push_back(entity);
    }

    if (!m_divided)
        return entityFound;

    std::vector<entt::entity> entityVec;

    entityVec = m_northWest->queryRange(reg, range);
    entityFound.insert(entityFound.end(), entityVec.begin(), entityVec.end());

    entityVec = m_northEast->queryRange(reg, range);
    entityFound.insert(entityFound.end(), entityVec.begin(), entityVec.end());

    entityVec = m_southEast->queryRange(reg, range);
    entityFound.insert(entityFound.end(), entityVec.begin(), entityVec.end());

    entityVec = m_southWest->queryRange(reg, range);
    entityFound.insert(entityFound.end(), entityVec.begin(), entityVec.end());

    return entityFound;
}

bool QuadTree::remove(entt::registry& reg, const entt::entity entityID)
{
    for (size_t i = 0; i < m_nodes.size(); ++i)
    {
        if (m_nodes[i] == entityID)
        {
            m_nodes.erase(m_nodes.begin() + i);
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
    if (!m_divided)
    {
        m_nodes.clear();
        return;
    }
    else
    {
        m_northWest->clear();
        m_northEast->clear();
        m_southEast->clear();
        m_southWest->clear();
    }

    if (!m_nodes.empty())
        m_nodes.clear();
}

void QuadTree::draw(sf::RenderTexture& rt)
{
    sf::RectangleShape border;
    border.setPosition(m_boundary.left, m_boundary.top);
    border.setSize(sf::Vector2f(m_boundary.width, m_boundary.height));
    border.setOutlineThickness(1.0f);
    border.setFillColor(sf::Color::Transparent);
    border.setOutlineColor(sf::Color(0, 150, 100));

    rt.draw(border);

    if (m_divided)
    {
        m_northWest->draw(rt);
        m_northEast->draw(rt);
        m_southEast->draw(rt);
        m_southWest->draw(rt);
    }
}