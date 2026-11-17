// Copyright 2026 The simple-engine-2 Authors.
//
// A small JSON document model for the editor's edits of component JSON
// (path points, script values): parse, change, write compactly. Numbers
// keep their source text, so values the editor does not touch pass back to
// the ComponentRegistry's hooks exactly as they came; objects keep their
// member order. glaze stays confined to the component units (ADR-010).

#ifndef ENGINE_SE_EDITOR_SRC_JSON_VALUE_H_
#define ENGINE_SE_EDITOR_SRC_JSON_VALUE_H_

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"

namespace se::editor::json {

enum class Kind : std::uint8_t {
  kNull,
  kBool,
  kNumber,
  kString,
  kArray,
  kObject,
};

struct Member;

struct Value {
  Kind kind = Kind::kNull;
  bool boolean = false;
  // A number's source text, or a string's decoded value.
  std::string text;
  std::vector<Value> items;
  std::vector<Member> members;

  [[nodiscard]] static Value Number(double number);
  // A float written with the shortest text that reads back as it.
  [[nodiscard]] static Value Float(float number);
  [[nodiscard]] static Value Object();

  // The member |key| of an object, or null.
  [[nodiscard]] const Value* Find(std::string_view key) const noexcept;
  [[nodiscard]] Value* Find(std::string_view key) noexcept;
  // Sets (or appends) the member |key|.
  void Set(std::string_view key, Value value);
  // A number as a double, or nullopt.
  [[nodiscard]] std::optional<double> AsDouble() const noexcept;
};

struct Member {
  std::string key;
  Value value;
};

[[nodiscard]] std::expected<Value, Error> Parse(std::string_view text);
[[nodiscard]] std::string Write(const Value& value);

// The same JSON value: objects member by member in any order, arrays in
// order, numbers by value (100 and 100.0 are equal).
[[nodiscard]] bool Equal(const Value& a, const Value& b);

}  // namespace se::editor::json

#endif  // ENGINE_SE_EDITOR_SRC_JSON_VALUE_H_
