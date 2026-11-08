// Copyright 2026 The simple-engine-2 Authors.
//
// The spatial hash finds exactly the brute-force pair set (R3) over seeded
// random worlds: varied sizes and clusters, boxes that straddle or touch
// cell edges, boxes that touch without overlapping, and entities far
// outside the arena.

#include "game_rampage/broadphase.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "se_core/guid.h"
#include "se_core/rng.h"
#include "se_core/types.h"

namespace se::rampage {
namespace {

using PairSet = std::set<std::pair<std::uint32_t, std::uint32_t>>;

PairSet BruteForce(const std::vector<Aabb>& boxes) {
  PairSet pairs;
  for (std::uint32_t i = 0; i < boxes.size(); ++i) {
    for (std::uint32_t j = 0; j < boxes.size(); ++j) {
      if (Overlaps(boxes[i], boxes[j])) pairs.emplace(i, j);
    }
  }
  return pairs;
}

PairSet ThroughHash(const std::vector<Aabb>& boxes, SpatialHash& hash) {
  hash.Build(boxes);
  PairSet pairs;
  std::vector<std::uint32_t> query;
  for (std::uint32_t i = 0; i < boxes.size(); ++i) {
    hash.Query(boxes[i], query);
    for (std::size_t k = 1; k < query.size(); ++k) {
      REQUIRE(query[k - 1] < query[k]);  // ascending, each once
    }
    for (const std::uint32_t j : query) {
      if (Overlaps(boxes[i], boxes[j])) pairs.emplace(i, j);
    }
  }
  return pairs;
}

class Draws {
 public:
  explicit Draws(std::uint64_t seed) : seed_(seed) {}
  float Unit() { return rng::Unit(rng::At(seed_, 0, Guid{}, 1, draw_++)); }
  float Range(float lo, float hi) { return lo + (hi - lo) * Unit(); }

 private:
  std::uint64_t seed_;
  std::uint32_t draw_ = 0;
};

std::vector<Aabb> RandomWorld(std::uint64_t seed) {
  Draws d(seed);
  std::vector<Aabb> boxes;
  const int clusters = 1 + static_cast<int>(d.Unit() * 6.0F);
  for (int c = 0; c < clusters; ++c) {
    const Vec2 centre{d.Range(-200.0F, 2100.0F), d.Range(-200.0F, 1300.0F)};
    const float spread = d.Range(10.0F, 600.0F);
    const int count = 5 + static_cast<int>(d.Unit() * 60.0F);
    for (int i = 0; i < count; ++i) {
      const Vec2 pos{centre.x + d.Range(-spread, spread),
                     centre.y + d.Range(-spread, spread)};
      // Mostly sprites, some tiny, some larger than a few cells.
      const float pick = d.Unit();
      const Vec2 half =
          pick < 0.1F   ? Vec2{d.Range(0.5F, 2.0F), 1.0F}
          : pick < 0.2F ? Vec2{d.Range(64.0F, 300.0F), d.Range(30.0F, 200.0F)}
                        : Vec2{24.0F, 24.0F};
      boxes.push_back(ColliderBox(pos, half));
    }
  }
  return boxes;
}

TEST_CASE("The spatial hash finds the brute-force pairs in random worlds",
          "[rampage][broadphase]") {
  SpatialHash hash;
  std::size_t total = 0;
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    INFO("seed " << seed);
    const std::vector<Aabb> boxes = RandomWorld(seed);
    const PairSet expected = BruteForce(boxes);
    CHECK(ThroughHash(boxes, hash) == expected);
    total += expected.size();
  }
  // The worlds have plenty of contacts, not just the self pairs.
  REQUIRE(total > 4000);
}

TEST_CASE("Boxes on cell edges, touching boxes and far-away boxes",
          "[rampage][broadphase]") {
  std::vector<Aabb> boxes;
  // Straddling and touching the 64 px cell edges.
  for (int i = -3; i <= 3; ++i) {
    const float edge = 64.0F * static_cast<float>(i);
    boxes.push_back(ColliderBox({edge, edge}, {24.0F, 24.0F}));
    boxes.push_back(ColliderBox({edge + 24.0F, 0.0F}, {24.0F, 24.0F}));
    boxes.push_back(ColliderBox({edge - 24.0F, 64.0F}, {24.0F, 24.0F}));
    // Exactly touching the previous box's right edge: no contact.
    boxes.push_back(ColliderBox({edge + 72.0F, 0.0F}, {24.0F, 24.0F}));
  }
  // Touching corners and edges without overlap.
  boxes.push_back(ColliderBox({500.0F, 500.0F}, {10.0F, 10.0F}));
  boxes.push_back(ColliderBox({520.0F, 500.0F}, {10.0F, 10.0F}));
  boxes.push_back(ColliderBox({520.0F, 520.0F}, {10.0F, 10.0F}));
  boxes.push_back(ColliderBox({500.0F, 520.0F}, {10.0F, 10.0F}));
  // Far outside the arena, alone and in pairs.
  boxes.push_back(ColliderBox({-1.0e7F, 3.0e7F}, {24.0F, 24.0F}));
  boxes.push_back(ColliderBox({-1.0e7F + 10.0F, 3.0e7F}, {24.0F, 24.0F}));
  boxes.push_back(ColliderBox({1.0e12F, -1.0e12F}, {24.0F, 24.0F}));
  boxes.push_back(ColliderBox({1.0e12F, -1.0e12F}, {1.0e6F, 1.0e6F}));
  boxes.push_back(ColliderBox({3.0e38F, 0.0F}, {24.0F, 24.0F}));
  // Larger than the oversized limit: always a candidate.
  boxes.push_back(ColliderBox({960.0F, 540.0F}, {5000.0F, 5000.0F}));

  const PairSet expected = BruteForce(boxes);
  SpatialHash hash;
  CHECK(ThroughHash(boxes, hash) == expected);

  // The touching pairs really are excluded and the overlapping ones found.
  const auto pair_at = [](Vec2 a, Vec2 b) {
    return Overlaps(ColliderBox(a, {10.0F, 10.0F}),
                    ColliderBox(b, {10.0F, 10.0F}));
  };
  CHECK_FALSE(pair_at({500.0F, 500.0F}, {520.0F, 500.0F}));
  CHECK_FALSE(pair_at({500.0F, 500.0F}, {520.0F, 520.0F}));
  CHECK(pair_at({500.0F, 500.0F}, {519.99F, 500.0F}));
  // The pair 1e7 px out overlaps.
  const auto far = static_cast<std::uint32_t>(boxes.size() - 6);
  CHECK(expected.contains({far, far + 1}));
}

TEST_CASE("Positive area is required on both axes", "[rampage][broadphase]") {
  const Aabb a = ColliderBox({0.0F, 0.0F}, {1.0F, 1.0F});
  CHECK(Overlaps(a, a));
  CHECK_FALSE(Overlaps(a, ColliderBox({2.0F, 0.0F}, {1.0F, 1.0F})));
  CHECK_FALSE(Overlaps(a, ColliderBox({0.0F, 2.0F}, {1.0F, 1.0F})));
  CHECK(Overlaps(a, ColliderBox({1.5F, 1.5F}, {1.0F, 1.0F})));
  // A zero-size box overlaps nothing, itself included.
  const Aabb point = ColliderBox({0.0F, 0.0F}, {0.0F, 0.0F});
  CHECK_FALSE(Overlaps(point, point));
  CHECK_FALSE(Overlaps(point, a));
}

TEST_CASE("Rebuilding reuses the buffers", "[rampage][broadphase]") {
  SpatialHash hash;
  const std::vector<Aabb> boxes = RandomWorld(7);
  std::vector<std::uint32_t> query;
  hash.Build(boxes);
  hash.Query(boxes[0], query);
  const PairSet first = ThroughHash(boxes, hash);
  // The same world again gives the same pairs.
  CHECK(ThroughHash(boxes, hash) == first);
  // A smaller world after a larger one.
  const std::vector<Aabb> small(boxes.begin(), boxes.begin() + 3);
  CHECK(ThroughHash(small, hash) == BruteForce(small));
}

}  // namespace
}  // namespace se::rampage
