#pragma once

#include <cstddef>
#include <random>
#include <vector>

#include <SFML/System/Vector2.hpp>

/**
 * @brief Waypoint paths of the editor's stress generator (Editor::generateEntities), shared with
 * legacy_replay --generate. The draws, in this order, are (docs/ledger.md):
 *
 *   d = std::uniform_int_distribution<std::uint32_t>(0, width)  (both bounds inclusive)
 *   per entity:  for (j = 0; j <= d(rng) % 10; ++j)  -- the bound is redrawn before every
 *                iteration, so a path has 1 to 10 points with P(more than n) = prod (1 - k/10)
 *   per point:   x = d(rng) % width, then y = d(rng) % height
 *
 * so x is uniform on 0..width-1 except that 0 is twice as likely, and y = d % height folds
 * 0..width onto 0..height-1 (at 1920x1080, y in 0..840 is twice as likely as 841..1079).
 * std::uniform_int_distribution is implementation-defined; the reference uses MSVC's STL.
*/
std::vector<std::vector<sf::Vector2f>> generateStressPaths(std::mt19937& rng, float width, float height, std::size_t count);
