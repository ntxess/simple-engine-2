// Copyright 2026 The simple-engine-2 Authors.
//
// What the background lane hands main (docs/design/mt-design.md "Data
// Flow": decoded payloads, or Error{code, message, path, line, column}).
// Payloads are values: the lane owns each buffer until it pushes the
// result into the completion ring, and main owns it from then on.
//
//   AtlasPage          a cooked atlas page (atlas_page.h), RGBA8
//   PcmPayload         a decoded sound (M8's audio plays it; the decoder
//                      comes from se_audio through SoundDecoder)
//   PrefabPayload      a prefab file's top-level members
//   SceneChunkPayload  one chunk of a scene's entities
//   SaveOutcome        a SaveRequest renamed into place (or superseded)
//   FileChange         the hot-reload scan saw a watched file change
//
// Prefabs and scene chunks are parsed with glaze on the lane
// (payloads_glaze.cc, the module's one glaze translation unit, ADR-082):
// each value keeps its JSON text (a prefab's compacted, a scene chunk's
// entities as written), which the ComponentRegistry's JSON hooks read on
// main.

#ifndef SE_ASSETS_PAYLOADS_H_
#define SE_ASSETS_PAYLOADS_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"

namespace se {

// A named rectangle of an atlas page, in pixels.
struct AtlasRegion {
  std::string name;
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  friend bool operator==(const AtlasRegion&, const AtlasRegion&) = default;
};

// A decoded atlas page: RGBA8, straight alpha, rows top to bottom.
struct AtlasPage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> pixels;
  std::vector<AtlasRegion> regions;
  // The source the page was cooked from: its size and SourceHash().
  std::uint64_t source_size = 0;
  std::uint64_t source_hash = 0;
  // Cooked on the lane from the source (a stale or missing page in a dev
  // build, or a hot reload) instead of read from a .seatlas file.
  bool recooked = false;

  [[nodiscard]] std::uint64_t Bytes() const noexcept {
    return std::uint64_t{width} * height * 4;
  }
};

// Interleaved 32-bit float samples.
struct PcmPayload {
  std::uint32_t channels = 0;
  std::uint32_t sample_rate = 0;
  std::uint64_t frames = 0;
  std::vector<float> samples;
};

struct JsonMember {
  std::string key;
  // The value's compact JSON text.
  std::string json;

  friend bool operator==(const JsonMember&, const JsonMember&) = default;
};

// A prefab file: a JSON object; its members in file order.
struct PrefabPayload {
  std::string path;
  std::vector<JsonMember> members;
};

// A scene chunk file (the importer splits a scene into chunks, M10):
//   { "format": "se-scene-chunk-v1", "chunk": N, "entities": [ {...} ] }
// Each entity's JSON text as written (syntax checked), in file order.
struct SceneChunkPayload {
  std::string path;
  std::uint32_t chunk = 0;
  std::vector<std::string> entities;
};

inline constexpr std::string_view kSceneChunkFormat = "se-scene-chunk-v1";

struct SaveOutcome {
  std::string path;
  std::uint64_t revision = 0;
  std::uint64_t bytes = 0;
  // A newer revision of the path was renamed into place first: nothing
  // was written over it.
  bool superseded = false;
  // Renames refused by a sharing violation before this one succeeded.
  std::uint32_t busy_retries = 0;
};

struct FileChange {
  std::string path;
  std::uint64_t tag = 0;
  bool exists = false;
  std::uint64_t size = 0;
  std::int64_t modify_time_ns = 0;
};

// Parses a prefab (a JSON object). Errors carry the line and column.
[[nodiscard]] std::expected<PrefabPayload, Error> ParsePrefab(
    std::string_view json, std::string_view path);
// Parses a scene chunk. Errors carry the line and column.
[[nodiscard]] std::expected<SceneChunkPayload, Error> ParseSceneChunk(
    std::string_view json, std::string_view path);
// Writes a scene chunk (tests, the M10 importer).
[[nodiscard]] std::expected<std::string, Error> WriteSceneChunk(
    std::uint32_t chunk, const std::vector<std::string>& entities);

}  // namespace se

#endif  // SE_ASSETS_PAYLOADS_H_
