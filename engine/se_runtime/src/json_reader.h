// Copyright 2026 The simple-engine-2 Authors.
//
// A small JSON reader for the runtime's settings files (config/config.json,
// config/sound.json): RFC 8259 documents into a tree of nodes. Objects keep
// their member order; a duplicate key is an error. Errors name the source
// and the line and column. glaze stays confined to the component units
// (ADR-010), and these files are read once at boot.

#ifndef ENGINE_SE_RUNTIME_SRC_JSON_READER_H_
#define ENGINE_SE_RUNTIME_SRC_JSON_READER_H_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"

namespace se::json_reader {

enum class Kind : std::uint8_t {
  kNull,
  kBool,
  kNumber,
  kString,
  kArray,
  kObject,
};

struct Member;

struct Node {
  Kind kind = Kind::kNull;
  bool boolean = false;
  double number = 0.0;
  // A string's decoded value.
  std::string text;
  std::vector<Node> items;
  std::vector<Member> members;

  // The member named |key| of an object, or nullptr.
  [[nodiscard]] const Node* Find(std::string_view key) const noexcept;
};

// An object's member.
struct Member {
  std::string name;
  Node value;
};

// Parses |text|; |source| names it in errors (a path).
[[nodiscard]] std::expected<Node, Error> Parse(std::string_view text,
                                               std::string_view source);

}  // namespace se::json_reader

#endif  // ENGINE_SE_RUNTIME_SRC_JSON_READER_H_
