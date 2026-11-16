// Copyright 2026 The simple-engine-2 Authors.
//
// TextureIds: how a panel shows a texture (docs/milestone-specs.md M10, the
// panel interface; docs/design/mt-design.md "Editor", textures in panels;
// ADR-095). Panels hold Handle<Texture> (a GpuResources slot, M7: a
// ViewportTarget's TargetTexture(), an asset's GpuTexture()) and ask for
// its ImTextureID every frame, in the Draw call that passes it to
// ImGui::Image. The id names the slot's resident texture of this frame
// only: a hot reload, a viewport resize or Stop releases it, so a panel
// never keeps one (checks.gpu_texture_members bans ImTextureID and
// SDL_GPUTexture* members in the editor, and a debug wrapper of ImGui's
// RenderDrawData asserts that no texture it binds was released this
// frame).
//
// The editor's TextureIds resolves through GpuResources and the SDL_GPU
// backend; panel tests give a fake.

#ifndef SE_EDITOR_TEXTURE_IDS_H_
#define SE_EDITOR_TEXTURE_IDS_H_

#include <imgui.h>

#include "se_core/handle.h"

namespace se::editor {

class TextureIds {
 public:
  TextureIds() = default;
  TextureIds(const TextureIds&) = delete;
  TextureIds& operator=(const TextureIds&) = delete;
  virtual ~TextureIds() = default;

  // |texture|'s ImTextureID for this frame. A stale, null or not yet
  // resident handle gives the magenta fallback's id, never an invalid one.
  [[nodiscard]] virtual ImTextureID Id(TextureHandle texture) = 0;
  // Its size in pixels (1 x 1 for the fallback).
  [[nodiscard]] virtual ImVec2 Size(TextureHandle texture) = 0;
};

}  // namespace se::editor

#endif  // SE_EDITOR_TEXTURE_IDS_H_
