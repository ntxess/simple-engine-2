// Copyright 2026 The simple-engine-2 Authors.
//
// The Viewport panel's entity overlays, ported from the legacy scene view's
// visualizers (app/editor/Editor.cpp:538-620 at master, the "Scene View
// Option" checkboxes of :265-276). The legacy editor drew them with SFML
// into the scene's render texture; the engine draws them as an ImGui
// overlay over the viewport image (docs/design/mt-design.md "Editor":
// gizmos and overlays are ImGui overlays), so they never touch the World or
// its render. BuildOverlays turns a const World into screen-space
// primitives the panel hands to ImGui's draw list; the tests check them
// without ImGui.
//
// Per entity with a Transform and a SpriteRenderer:
//   - ids: the entity's Guid (its first 8 hex digits) at the bottom-right
//     corner of its sprite bounds (the legacy drew the EnTT id there);
//   - boxes: its collision box (rampage's Collider, rule R4's unrotated box)
//     or, without a Collider, its sprite bounds; red when it overlaps the
//     player's box (an entity with PlayerControl, not itself), else green
//     (the legacy outlined the sprite bounds, which were its hitbox);
//   - headings: 50 world-pixel lines from its position along its rotation
//     (east) and a quarter turn before it (north);
//   - positions: "x, y" to two decimals at the right edge of its bounds.

#ifndef SE_EDITOR_PANELS_OVERLAYS_H_
#define SE_EDITOR_PANELS_OVERLAYS_H_

#include <array>
#include <string>
#include <vector>

#include "se_core/types.h"
#include "se_core/world.h"
#include "se_editor_panels/viewport_math.h"

namespace se::editor {

struct OverlayOptions {
  bool ids = false;
  bool boxes = false;
  bool headings = false;
  bool positions = false;

  [[nodiscard]] bool Any() const noexcept {
    return ids || boxes || headings || positions;
  }
};

inline constexpr Rgba8 kOverlayWhite{255, 255, 255, 255};
inline constexpr Rgba8 kOverlayRed{255, 0, 0, 255};
inline constexpr Rgba8 kOverlayGreen{0, 255, 0, 255};
inline constexpr float kHeadingLength = 50.0F;

// A closed outline (four screen-space corners).
struct OverlayQuad {
  std::array<Vec2, 4> corners;
  Rgba8 color;
};

struct OverlayLine {
  Vec2 from;
  Vec2 to;
  Rgba8 color;
};

struct OverlayText {
  Vec2 pos;
  std::string text;
  Rgba8 color = kOverlayWhite;
};

struct Overlays {
  std::vector<OverlayQuad> quads;
  std::vector<OverlayLine> lines;
  std::vector<OverlayText> texts;

  void Clear() {
    quads.clear();
    lines.clear();
    texts.clear();
  }
};

// Replaces |out| with the overlays of |world| as |mapping| shows it.
void BuildOverlays(const World& world, const OverlayOptions& options,
                   const ViewportMapping& mapping, Overlays& out);

}  // namespace se::editor

#endif  // SE_EDITOR_PANELS_OVERLAYS_H_
