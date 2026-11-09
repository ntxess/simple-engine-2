// Copyright 2026 The simple-engine-2 Authors.

#include "game_rampage/broadphase.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "se_core/types.h"

namespace se::rampage {
namespace {

// Cell coordinates are clamped to +-2^30, so far-away boxes share the edge
// cells (still a superset) and keys never overflow.
constexpr double kCellLimit = 1073741824.0;

std::int64_t CellOf(float v, float cell) {
  const double c = std::floor(static_cast<double>(v) / cell);
  return static_cast<std::int64_t>(std::clamp(c, -kCellLimit, kCellLimit));
}

std::uint64_t KeyOf(std::int64_t x, std::int64_t y) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
         static_cast<std::uint32_t>(y);
}

// sf::Rect::findIntersection's helpers.
float Min(float a, float b) { return (a < b) ? a : b; }
float Max(float a, float b) { return (a < b) ? b : a; }

}  // namespace

Aabb ColliderBox(Vec2 pos, Vec2 half) noexcept {
  const Vec2 size{half.x * 2.0F, half.y * 2.0F};
  Aabb box;
  box.min = {pos.x - half.x, pos.y - half.y};
  box.max = {box.min.x + size.x, box.min.y + size.y};
  return box;
}

bool Overlaps(const Aabb& a, const Aabb& b) noexcept {
  // The rectangles as findIntersection normalises them.
  const float a_min_x = Min(a.min.x, a.max.x);
  const float a_max_x = Max(a.min.x, a.max.x);
  const float a_min_y = Min(a.min.y, a.max.y);
  const float a_max_y = Max(a.min.y, a.max.y);
  const float b_min_x = Min(b.min.x, b.max.x);
  const float b_max_x = Max(b.min.x, b.max.x);
  const float b_min_y = Min(b.min.y, b.max.y);
  const float b_max_y = Max(b.min.y, b.max.y);
  const float left = Max(a_min_x, b_min_x);
  const float top = Max(a_min_y, b_min_y);
  const float right = Min(a_max_x, b_max_x);
  const float bottom = Min(a_max_y, b_max_y);
  return left < right && top < bottom;
}

SpatialHash::CellRange SpatialHash::RangeOf(const Aabb& box) const noexcept {
  CellRange range{};
  const bool finite = std::isfinite(box.min.x) && std::isfinite(box.min.y) &&
                      std::isfinite(box.max.x) && std::isfinite(box.max.y);
  if (!finite) {
    range.oversized = true;
    return range;
  }
  const float lo_x = std::min(box.min.x, box.max.x);
  const float hi_x = std::max(box.min.x, box.max.x);
  const float lo_y = std::min(box.min.y, box.max.y);
  const float hi_y = std::max(box.min.y, box.max.y);
  range.x0 = CellOf(lo_x, cell_);
  range.x1 = CellOf(hi_x, cell_);
  range.y0 = CellOf(lo_y, cell_);
  range.y1 = CellOf(hi_y, cell_);
  const std::int64_t cells =
      (range.x1 - range.x0 + 1) * (range.y1 - range.y0 + 1);
  range.oversized = cells > kMaxCellsPerBox;
  return range;
}

void SpatialHash::Build(std::span<const Aabb> boxes) {
  entries_.clear();
  oversized_.clear();
  count_ = static_cast<std::uint32_t>(boxes.size());
  for (std::uint32_t i = 0; i < count_; ++i) {
    const CellRange range = RangeOf(boxes[i]);
    if (range.oversized) {
      oversized_.push_back(i);
      continue;
    }
    for (std::int64_t x = range.x0; x <= range.x1; ++x) {
      for (std::int64_t y = range.y0; y <= range.y1; ++y) {
        entries_.push_back(Entry{KeyOf(x, y), i});
      }
    }
  }
  std::sort(entries_.begin(), entries_.end(),
            [](const Entry& a, const Entry& b) {
              return a.key != b.key ? a.key < b.key : a.index < b.index;
            });
  if (stamp_.size() < count_) stamp_.resize(count_, 0);
}

void SpatialHash::Query(const Aabb& box, std::vector<std::uint32_t>& out) {
  out.clear();
  const CellRange range = RangeOf(box);
  if (range.oversized) {
    for (std::uint32_t i = 0; i < count_; ++i) out.push_back(i);
    return;
  }
  if (++generation_ == 0) {
    std::fill(stamp_.begin(), stamp_.end(), 0);
    generation_ = 1;
  }
  const auto add = [this, &out](std::uint32_t index) {
    if (stamp_[index] == generation_) return;
    stamp_[index] = generation_;
    out.push_back(index);
  };
  for (std::int64_t x = range.x0; x <= range.x1; ++x) {
    for (std::int64_t y = range.y0; y <= range.y1; ++y) {
      const std::uint64_t key = KeyOf(x, y);
      auto it = std::lower_bound(
          entries_.begin(), entries_.end(), key,
          [](const Entry& entry, std::uint64_t k) { return entry.key < k; });
      for (; it != entries_.end() && it->key == key; ++it) add(it->index);
    }
  }
  for (const std::uint32_t index : oversized_) add(index);
  std::sort(out.begin(), out.end());
}

}  // namespace se::rampage
