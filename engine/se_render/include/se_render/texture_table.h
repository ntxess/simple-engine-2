// Copyright 2026 The simple-engine-2 Authors.
//
// The texture table (ADR-026): config/texture.json maps texture names, the
// names scenes give SpriteRenderer::texture, to PNG paths relative to the
// content root:
//
//   { "player": "assets/player/player.png", "coin": "assets/coin/coin.png" }
//
// LoadTextureTable registers every name with GpuResources, decodes its PNG
// with stb_image on main and queues the upload (M7). Since M9 the App
// loads the table's textures from cooked pages through the background
// lane (se_runtime/asset_runtime.h) and uses only ParseTextureTable;
// LoadTextureTable serves tests and tools (render_shot). A file that is missing
// or does not decode is an Error naming its path in the report, and sprites of
// that texture draw magenta; only an unreadable or malformed table fails the
// call.

#ifndef SE_RENDER_TEXTURE_TABLE_H_
#define SE_RENDER_TEXTURE_TABLE_H_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_render/gpu_resources.h"

namespace se {

struct TextureTableEntry {
  std::string name;
  // As written in the table (relative to the content root).
  std::string path;
};

// Parses a JSON object whose values are strings, in file order. |source|
// names the table in errors (with line and column).
[[nodiscard]] std::expected<std::vector<TextureTableEntry>, Error>
ParseTextureTable(std::string_view json, std::string_view source);

struct TextureLoadReport {
  // Every entry of the table, its path joined to the content root.
  std::vector<TextureTableEntry> entries;
  std::uint32_t loaded = 0;
  // One per texture whose file is missing or does not decode.
  std::vector<Error> errors;
};

// Loads |table_path| (relative to |content_root|, or absolute).
[[nodiscard]] std::expected<TextureLoadReport, Error> LoadTextureTable(
    GpuResources& resources, std::string_view content_root,
    std::string_view table_path) SE_REQUIRES(g_main);

// Decodes |path| and queues it as |handle|'s next texture (the reload
// path: --reload-texture, M9's hot reload).
[[nodiscard]] std::expected<void, Error> LoadTextureFile(
    GpuResources& resources, TextureHandle handle, std::string_view path)
    SE_REQUIRES(g_main);

}  // namespace se

#endif  // SE_RENDER_TEXTURE_TABLE_H_
