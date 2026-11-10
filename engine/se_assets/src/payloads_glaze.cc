// Copyright 2026 The simple-engine-2 Authors.
//
// The one glaze translation unit of se_assets (ADR-082), run on the
// background lane: prefabs are parsed into glaze's generic JSON (64-bit
// integers kept exact, members in file order) and each member written back
// as compact JSON; scene chunks are read by reflection, each entity's text
// kept as written. Main hands the texts to the ComponentRegistry's hooks.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <glaze/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_assets/payloads.h"
#include "se_core/error.h"

namespace se {
namespace payloads_internal {

// A scene chunk file, read by reflection after glaze's validator has
// checked the whole text: each entity's text is kept as written, which a
// parse into the generic value and back would cost many times over (a
// chunk is one lane step, bench.lane_steps).
struct ChunkFile {
  std::string format;
  std::optional<std::uint32_t> chunk;
  std::optional<std::vector<glz::raw_json>> entities;
};

}  // namespace payloads_internal

namespace {

using Json = glz::generic_u64;

// The 1-based line and column of byte |offset| of |text|.
void LineColumn(std::string_view text, std::size_t offset, std::int32_t& line,
                std::int32_t& column) {
  line = 1;
  column = 1;
  for (std::size_t i = 0; i < offset && i < text.size(); ++i) {
    if (text[i] == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
  }
}

std::unexpected<Error> ParseError(std::string_view what,
                                  const std::string& text,
                                  const glz::error_ctx& error,
                                  std::string_view path) {
  Error out{ErrorCode::kParse,
            std::format("{}: {}", what, glz::format_error(error, text)),
            std::string(path), 0, 0};
  LineColumn(text, error.count, out.line, out.column);
  return std::unexpected(std::move(out));
}

std::unexpected<Error> ShapeError(std::string message, std::string_view path) {
  return std::unexpected(
      Error{ErrorCode::kParse, std::move(message), std::string(path), 1, 1});
}

std::expected<Json, Error> Read(std::string_view json, std::string_view what,
                                std::string_view path, std::string& text) {
  // glaze reads null-terminated buffers.
  text.assign(json);
  Json document;
  if (const glz::error_ctx error = glz::read_json(document, text); error) {
    // Reading into the generic value may backtrack to its start; the
    // validator reports where the text stops being JSON.
    const glz::error_ctx where = glz::validate_json(text);
    return ParseError(what, text, where ? where : error, path);
  }
  return document;
}

std::expected<std::string, Error> Compact(const Json& value,
                                          std::string_view path) {
  std::string out;
  if (const glz::error_ctx error = glz::write_json(value, out); error) {
    return std::unexpected(
        Error{ErrorCode::kParse,
              "cannot write a value back as JSON: " + glz::format_error(error),
              std::string(path), 0, 0});
  }
  return out;
}

}  // namespace

std::expected<PrefabPayload, Error> ParsePrefab(std::string_view json,
                                                std::string_view path) {
  std::string text;
  auto document = Read(json, "prefab", path, text);
  if (!document) return std::unexpected(std::move(document.error()));
  if (!document->holds<Json::object_t>()) {
    return ShapeError("a prefab is a JSON object", path);
  }
  PrefabPayload prefab;
  prefab.path = std::string(path);
  for (const auto& [key, value] : document->get<Json::object_t>()) {
    auto compact = Compact(value, path);
    if (!compact) return std::unexpected(std::move(compact.error()));
    prefab.members.push_back({std::string(key), std::move(*compact)});
  }
  return prefab;
}

std::expected<SceneChunkPayload, Error> ParseSceneChunk(std::string_view json,
                                                        std::string_view path) {
  // glaze reads null-terminated buffers.
  const std::string text(json);
  // The whole text is valid JSON (skipping a raw value does not check
  // it), then the shape is read.
  if (const glz::error_ctx invalid = glz::validate_json(text); invalid) {
    return ParseError("scene chunk", text, invalid, path);
  }
  payloads_internal::ChunkFile file;
  if (const glz::error_ctx error = glz::read_json(file, text); error) {
    return ParseError("scene chunk", text, error, path);
  }
  if (file.format != kSceneChunkFormat) {
    return ShapeError(std::format("a scene chunk names \"format\": \"{}\"",
                                  kSceneChunkFormat),
                      path);
  }
  if (!file.entities) {
    return ShapeError("a scene chunk has an \"entities\" array", path);
  }
  SceneChunkPayload chunk;
  chunk.path = std::string(path);
  chunk.chunk = file.chunk.value_or(0);
  chunk.entities.reserve(file.entities->size());
  for (glz::raw_json& entity : *file.entities) {
    if (entity.str.empty() || entity.str.front() != '{') {
      return ShapeError("every entity of a scene chunk is a JSON object", path);
    }
    chunk.entities.push_back(std::move(entity.str));
  }
  return chunk;
}

std::expected<std::string, Error> WriteSceneChunk(
    std::uint32_t chunk, const std::vector<std::string>& entities) {
  std::string out =
      std::format("{{\"format\":\"{}\",\"chunk\":{},\"entities\":[",
                  kSceneChunkFormat, chunk);
  for (std::size_t i = 0; i < entities.size(); ++i) {
    if (i > 0) out += ',';
    out += entities[i];
  }
  out += "]}";
  // The entities must be JSON objects: parse what was written.
  if (auto parsed = ParseSceneChunk(out, "scene chunk"); !parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  return out;
}

}  // namespace se
