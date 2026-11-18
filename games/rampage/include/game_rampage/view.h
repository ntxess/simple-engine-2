// Copyright 2026 The simple-engine-2 Authors.
//
// How the sandbox is drawn. The legacy Sandbox drew its entities into a
// scene render texture of the configured resolution (SceneViewRenderer), so
// its view was the whole arena at one canvas pixel per world pixel. Scenes
// that need another view place an entity with a primary Camera2D.

#ifndef GAME_RAMPAGE_VIEW_H_
#define GAME_RAMPAGE_VIEW_H_

#include "game_rampage/components.h"
#include "se_core/world.h"
#include "se_render_core/render_components.h"

namespace se::rampage {

// The whole arena: centred on it, zoom 1, its size as the canvas.
[[nodiscard]] Camera2D ArenaCamera(const ArenaBounds& bounds) noexcept;

// The World's primary Camera2D (PrimaryCamera), or the ArenaCamera of its
// ArenaBounds (the default bounds when the World has none).
[[nodiscard]] Camera2D SandboxCamera(const World& world);

}  // namespace se::rampage

#endif  // GAME_RAMPAGE_VIEW_H_
