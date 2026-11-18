#include "StressGenerator.hpp"

std::vector<std::vector<sf::Vector2f>> generateStressPaths(std::mt19937& rng, float width, float height, std::size_t count)
{
    std::vector<std::vector<sf::Vector2f>> paths;
    paths.reserve(count);

    // The draws below are exactly those of the original Editor::generateEntities.
    std::uniform_int_distribution<std::mt19937::result_type> dist6(0, static_cast<unsigned int>(width));
    for (std::size_t i = 0; i < count; i++)
    {
        std::vector<sf::Vector2f> points;
        for (std::size_t j = 0; j <= std::size_t(dist6(rng)) % 10; j++)
        {
            // Braced initialisation evaluates x before y.
            points.push_back(sf::Vector2f{ float(dist6(rng) % int(width)), float(dist6(rng) % int(height)) });
        }
        paths.push_back(std::move(points));
    }

    return paths;
}
