// Copyright 2026 The simple-engine-2 Authors.
//
// DebugDraw: a World's sink of debug primitives (lines, boxes and
// points in world units, with a colour) that the tick's systems write and a
// viewer draws (docs/design/costfree-end-state.md, the ctx resources;
// docs/design/mt-design.md "Rules inside a fork"; docs/milestone-specs.md
// M14, ADR-113).
//
// A DebugDraw is World-level: its writes assert not-in-fork, as every
// World-level sink's do. A system that forks gives each chunk its slice
// instead: DebugDrawSlices holds one arena per chunk (jobs::ChunkOutputs),
// a chunk writes only its own (At(range)), and the owner merges the slices
// into the DebugDraw in chunk order after the join, so the primitives are
// the same, in the same order, for every worker count and claim order:
//
//   slices.Reset(pool.ChunkCountFor(site, n));      // owner, before
//   pool.ParallelFor(site, n, [&](jobs::ChunkRange r) {
//     jobs::DebugDrawSlice slice = slices.At(r);   // the chunk's own
//     slice.Line(a, b, colour);
//   });
//   slices.MergeInto(world_debug_draw);             // owner, after
//
// Nothing here draws: the primitives are data (a renderer or the editor's
// Viewport overlays read Primitives()).

#ifndef SE_CORE_DEBUG_DRAW_H_
#define SE_CORE_DEBUG_DRAW_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "se_core/jobs.h"
#include "se_core/types.h"

namespace se {

enum class DebugPrimitiveKind : std::uint8_t {
  kLine,   // from a to b
  kBox,    // the axis-aligned box from a (min) to b (max)
  kPoint,  // at a
};

struct DebugPrimitive {
  DebugPrimitiveKind kind = DebugPrimitiveKind::kLine;
  Vec2 a;
  Vec2 b;
  Rgba8 color;

  friend bool operator==(const DebugPrimitive&,
                         const DebugPrimitive&) = default;
};

// The calls a system writes with, into |out|.
class DebugDrawWriter {
 public:
  explicit DebugDrawWriter(std::vector<DebugPrimitive>& out) noexcept
      : out_(&out) {}

  void Line(Vec2 from, Vec2 to, Rgba8 color) {
    out_->push_back({DebugPrimitiveKind::kLine, from, to, color});
  }
  void Box(Vec2 min, Vec2 max, Rgba8 color) {
    out_->push_back({DebugPrimitiveKind::kBox, min, max, color});
  }
  void Point(Vec2 at, Rgba8 color) {
    out_->push_back({DebugPrimitiveKind::kPoint, at, at, color});
  }

 private:
  std::vector<DebugPrimitive>* out_;
};

// The World-level sink: owner thread, never inside a fork chunk.
class DebugDraw {
 public:
  void Line(Vec2 from, Vec2 to, Rgba8 color);
  void Box(Vec2 min, Vec2 max, Rgba8 color);
  void Point(Vec2 at, Rgba8 color);
  // Appends |primitives| in order (a merge of slices).
  void Append(std::span<const DebugPrimitive> primitives);
  // The tick's primitives start over.
  void Clear();

  [[nodiscard]] std::span<const DebugPrimitive> Primitives() const noexcept {
    return primitives_;
  }
  [[nodiscard]] std::size_t size() const noexcept { return primitives_.size(); }

 private:
  std::vector<DebugPrimitive> primitives_;
};

namespace jobs {

// A chunk's slice of a DebugDraw: the same calls, into the chunk's arena.
using DebugDrawSlice = DebugDrawWriter;

// Per-chunk DebugDraw slices for one fork (mt-design "Rules inside a
// fork"). Reset and MergeInto on the owner thread, outside the fork; At
// inside it, from the running chunk only.
class DebugDrawSlices {
 public:
  // Owner thread, before the fork: |chunks| empty slices (the arenas keep
  // their capacity).
  void Reset(std::uint32_t chunks) { outputs_.Reset(chunks); }
  // Inside the fork: the running chunk's slice.
  [[nodiscard]] DebugDrawSlice At(const ChunkRange& range) {
    return DebugDrawSlice(outputs_.At(range));
  }
  [[nodiscard]] std::uint32_t Chunks() const noexcept {
    return outputs_.Chunks();
  }
  [[nodiscard]] std::size_t Total() const noexcept { return outputs_.Total(); }
  // Owner thread, after the join: every slice into |sink|, in chunk order.
  void MergeInto(DebugDraw& sink) const;

 private:
  ChunkOutputs<DebugPrimitive> outputs_;
};

}  // namespace jobs
}  // namespace se

#endif  // SE_CORE_DEBUG_DRAW_H_
