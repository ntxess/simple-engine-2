// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/debug_draw.h"

#include <cstdint>
#include <span>

#include "se_core/assert.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"

namespace se {

void DebugDraw::Line(Vec2 from, Vec2 to, Rgba8 color) {
  SE_ASSERT_NOT_IN_FORK();
  DebugDrawWriter(primitives_).Line(from, to, color);
}

void DebugDraw::Box(Vec2 min, Vec2 max, Rgba8 color) {
  SE_ASSERT_NOT_IN_FORK();
  DebugDrawWriter(primitives_).Box(min, max, color);
}

void DebugDraw::Point(Vec2 at, Rgba8 color) {
  SE_ASSERT_NOT_IN_FORK();
  DebugDrawWriter(primitives_).Point(at, color);
}

void DebugDraw::Append(std::span<const DebugPrimitive> primitives) {
  SE_ASSERT_NOT_IN_FORK();
  primitives_.insert(primitives_.end(), primitives.begin(), primitives.end());
}

void DebugDraw::Clear() {
  SE_ASSERT_NOT_IN_FORK();
  primitives_.clear();
}

namespace jobs {

void DebugDrawSlices::MergeInto(DebugDraw& sink) const {
  for (std::uint32_t i = 0; i < outputs_.Chunks(); ++i) {
    sink.Append(outputs_.Chunk(i));
  }
}

}  // namespace jobs
}  // namespace se
