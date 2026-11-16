// Copyright 2026 The simple-engine-2 Authors.
//
// Collision geometry: the collider box of R4, the positive-area overlap test
// of R3, and the spatial-hash broadphase the CollisionSystem uses to find
// candidate pairs (tests/rampage/broadphase_test.cc checks it against brute
// force). The hash is built in two halves so that a fork site can make the
// cells of each box per chunk (AppendCells) and the owner sorts them
// (Assemble); queries are const, so chunks may query concurrently.

#ifndef GAME_RAMPAGE_BROADPHASE_H_
#define GAME_RAMPAGE_BROADPHASE_H_

#include <cstdint>
#include <span>
#include <vector>

#include "se_core/jobs.h"
#include "se_core/types.h"

namespace se::rampage {

struct Aabb {
  Vec2 min;
  Vec2 max;
};

// The collider of R4: the unrotated box of size 2 * half centred on |pos|,
// computed as the legacy sf::FloatRect{pos - size / 2, size} is, so min =
// pos - half and max = min + size.
[[nodiscard]] Aabb ColliderBox(Vec2 pos, Vec2 half) noexcept;

// True when the boxes intersect with positive area (touching edges do not
// count), with sf::Rect::findIntersection's comparisons.
[[nodiscard]] bool Overlaps(const Aabb& a, const Aabb& b) noexcept;

// A uniform grid over the plane, rebuilt every tick. Build, Assemble and
// Query reuse their buffers, so steady-state ticks do not allocate.
class SpatialHash {
 public:
  static constexpr float kDefaultCell = 64.0F;
  // A box covering more cells than this, or with a non-finite coordinate,
  // is kept in a list every query returns.
  static constexpr std::int64_t kMaxCellsPerBox = 256;

  // One box in one cell.
  struct Entry {
    std::uint64_t key;
    std::uint32_t index;
  };

  explicit SpatialHash(float cell = kDefaultCell) noexcept : cell_(cell) {}

  void Build(std::span<const Aabb> boxes);
  // The first half of Build for box |index|: its cells into |entries|, or
  // the index into |oversized|. Any thread.
  void AppendCells(const Aabb& box, std::uint32_t index,
                   std::vector<Entry>& entries,
                   std::vector<std::uint32_t>& oversized) const;
  // The second half: the hash of |count| boxes from the per-chunk outputs
  // of AppendCells (chunks in order, each in index order). The same hash as
  // Build of the same boxes.
  void Assemble(std::uint32_t count, const jobs::ChunkOutputs<Entry>& entries,
                const jobs::ChunkOutputs<std::uint32_t>& oversized);
  // Replaces |out| with the indices of the boxes that share a cell with
  // |box|, ascending, each once: a superset of the boxes |box| overlaps.
  // Const, so concurrent queries are safe.
  void Query(const Aabb& box, std::vector<std::uint32_t>& out) const;

  [[nodiscard]] float Cell() const noexcept { return cell_; }

 private:
  struct CellRange {
    std::int64_t x0;
    std::int64_t y0;
    std::int64_t x1;
    std::int64_t y1;
    bool oversized;
  };

  [[nodiscard]] CellRange RangeOf(const Aabb& box) const noexcept;
  void SortEntries();

  float cell_;
  std::uint32_t count_ = 0;
  // Sorted by (key, index).
  std::vector<Entry> entries_;
  std::vector<std::uint32_t> oversized_;
};

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_BROADPHASE_H_
