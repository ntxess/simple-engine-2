// Copyright 2026 The simple-engine-2 Authors.
//
// A small JSON document model for scene, prefab and scenario files: parse,
// RFC 7386 merge patches and writing. Component values themselves are read
// and written by the ComponentRegistry's JSON hooks (glaze, confined to the
// *_components.cc units, ADR-010); this model only splits files into
// per-component texts and merges them.
//
// Numbers keep their source text, so a value passes to the component hooks
// exactly as written. Objects keep their member order; duplicate keys are a
// parse error.

#ifndef GAME_RAMPAGE_JSON_H_
#define GAME_RAMPAGE_JSON_H_

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"

namespace se::rampage::json {

enum class Kind : std::uint8_t {
  kNull,
  kBool,
  kNumber,
  kString,
  kArray,
  kObject,
};

struct Member;

class Value {
 public:
  Value() = default;

  [[nodiscard]] static Value Bool(bool value);
  // |text| must be a JSON number.
  [[nodiscard]] static Value Number(std::string text);
  [[nodiscard]] static Value NumberOf(double value);
  [[nodiscard]] static Value NumberOf(std::int64_t value);
  [[nodiscard]] static Value String(std::string text);
  [[nodiscard]] static Value Array();
  [[nodiscard]] static Value Object();

  [[nodiscard]] Kind GetKind() const noexcept { return kind_; }
  [[nodiscard]] bool IsNull() const noexcept { return kind_ == Kind::kNull; }
  [[nodiscard]] bool IsBool() const noexcept { return kind_ == Kind::kBool; }
  [[nodiscard]] bool IsNumber() const noexcept {
    return kind_ == Kind::kNumber;
  }
  [[nodiscard]] bool IsString() const noexcept {
    return kind_ == Kind::kString;
  }
  [[nodiscard]] bool IsArray() const noexcept { return kind_ == Kind::kArray; }
  [[nodiscard]] bool IsObject() const noexcept {
    return kind_ == Kind::kObject;
  }

  [[nodiscard]] bool AsBool() const noexcept { return bool_; }
  // The source text of a number, or of a string's decoded value.
  [[nodiscard]] const std::string& Text() const noexcept { return text_; }
  // A number as a double (correctly rounded), or nullopt.
  [[nodiscard]] std::optional<double> AsDouble() const noexcept;
  // A number written as an integer that fits, or nullopt.
  [[nodiscard]] std::optional<std::int64_t> AsInt64() const noexcept;

  [[nodiscard]] const std::vector<Value>& Items() const noexcept {
    return items_;
  }
  [[nodiscard]] std::vector<Value>& MutableItems() noexcept { return items_; }
  [[nodiscard]] const std::vector<Member>& Members() const noexcept {
    return members_;
  }
  [[nodiscard]] std::vector<Member>& MutableMembers() noexcept {
    return members_;
  }

  // Object members by key; nullptr when absent or not an object.
  [[nodiscard]] const Value* Find(std::string_view key) const noexcept;
  [[nodiscard]] Value* Find(std::string_view key) noexcept;
  // Replaces or appends a member (makes a null value an object).
  void Set(std::string_view key, Value value);
  // Removes a member; false when absent.
  bool Erase(std::string_view key);
  // Appends to an array (makes a null value an array).
  void Push(Value value);

 private:
  Kind kind_ = Kind::kNull;
  bool bool_ = false;
  std::string text_;
  std::vector<Value> items_;
  std::vector<Member> members_;
};

struct Member {
  std::string key;
  Value value;
};

// Parses one JSON document (RFC 8259). Errors carry the line and column.
[[nodiscard]] std::expected<Value, Error> Parse(std::string_view text);

// Writes |value| compactly, or indented by |indent| spaces per level.
[[nodiscard]] std::string Write(const Value& value, int indent = 0);

// Applies an RFC 7386 merge patch: objects merge member by member, null
// removes a member, anything else (arrays included) replaces the target
// whole.
void MergePatch(Value& target, const Value& patch);

}  // namespace se::rampage::json

#endif  // GAME_RAMPAGE_JSON_H_
