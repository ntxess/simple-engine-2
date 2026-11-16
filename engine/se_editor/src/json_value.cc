// Copyright 2026 The simple-engine-2 Authors.

#include "src/json_value.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "se_core/error.h"

namespace se::editor::json {
namespace {

constexpr int kMaxDepth = 64;

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  std::expected<Value, Error> Document() {
    auto value = ParseValue(0);
    if (!value) return value;
    SkipSpace();
    if (pos_ != text_.size()) return Failure("trailing characters");
    return value;
  }

 private:
  std::unexpected<Error> Failure(std::string_view what) const {
    return Fail(ErrorCode::kParse,
                std::format("JSON: {} at offset {}", what, pos_));
  }

  void SkipSpace() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
            text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool Consume(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) return false;
    pos_ += word.size();
    return true;
  }

  std::expected<Value, Error> ParseValue(int depth) {
    if (depth > kMaxDepth) return Failure("nesting too deep");
    SkipSpace();
    if (pos_ >= text_.size()) return Failure("unexpected end");
    Value value;
    const char ch = text_[pos_];
    if (ch == '{') return ParseObject(depth);
    if (ch == '[') return ParseArray(depth);
    if (ch == '"') {
      auto text = ParseString();
      if (!text) return std::unexpected(std::move(text.error()));
      value.kind = Kind::kString;
      value.text = std::move(*text);
      return value;
    }
    if (Consume("true")) {
      value.kind = Kind::kBool;
      value.boolean = true;
      return value;
    }
    if (Consume("false")) {
      value.kind = Kind::kBool;
      return value;
    }
    if (Consume("null")) return value;
    return ParseNumber();
  }

  std::expected<Value, Error> ParseNumber() {
    const std::size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    while (pos_ < text_.size() &&
           ((text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '.' ||
            text_[pos_] == 'e' || text_[pos_] == 'E' || text_[pos_] == '+' ||
            text_[pos_] == '-')) {
      ++pos_;
    }
    if (pos_ == start) return Failure("unexpected character");
    Value value;
    value.kind = Kind::kNumber;
    value.text = std::string(text_.substr(start, pos_ - start));
    if (!value.AsDouble()) return Failure("malformed number");
    return value;
  }

  std::expected<std::string, Error> ParseString() {
    ++pos_;  // the opening quote
    std::string out;
    while (pos_ < text_.size()) {
      const char ch = text_[pos_++];
      if (ch == '"') return out;
      if (ch != '\\') {
        out.push_back(ch);
        continue;
      }
      if (pos_ >= text_.size()) break;
      const char escape = text_[pos_++];
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          out.push_back(escape);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (pos_ + 4 > text_.size()) return Failure("short \\u escape");
          std::uint32_t code = 0;
          const auto [end, error] = std::from_chars(
              text_.data() + pos_, text_.data() + pos_ + 4, code, 16);
          if (error != std::errc() || end != text_.data() + pos_ + 4) {
            return Failure("bad \\u escape");
          }
          pos_ += 4;
          // UTF-8 for the basic plane (no surrogate pairs in component
          // values).
          if (code < 0x80) {
            out.push_back(static_cast<char>(code));
          } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          }
          break;
        }
        default:
          return Failure("bad escape");
      }
    }
    return Failure("unterminated string");
  }

  std::expected<Value, Error> ParseArray(int depth) {
    ++pos_;
    Value value;
    value.kind = Kind::kArray;
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return value;
    }
    while (true) {
      auto item = ParseValue(depth + 1);
      if (!item) return item;
      value.items.push_back(std::move(*item));
      SkipSpace();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == ']') {
        ++pos_;
        return value;
      }
      return Failure("expected ',' or ']'");
    }
  }

  std::expected<Value, Error> ParseObject(int depth) {
    ++pos_;
    Value value;
    value.kind = Kind::kObject;
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return value;
    }
    while (true) {
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        return Failure("expected a key");
      }
      auto key = ParseString();
      if (!key) return std::unexpected(std::move(key.error()));
      if (value.Find(*key) != nullptr) return Failure("duplicate key");
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != ':') {
        return Failure("expected ':'");
      }
      ++pos_;
      auto member = ParseValue(depth + 1);
      if (!member) return member;
      value.members.push_back(Member{std::move(*key), std::move(*member)});
      SkipSpace();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == '}') {
        ++pos_;
        return value;
      }
      return Failure("expected ',' or '}'");
    }
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

void WriteString(std::string_view text, std::string& out) {
  out.push_back('"');
  for (const char ch : text) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out += std::format("\\u{:04x}", static_cast<unsigned char>(ch));
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

void WriteValue(const Value& value, std::string& out) {
  switch (value.kind) {
    case Kind::kNull:
      out += "null";
      return;
    case Kind::kBool:
      out += value.boolean ? "true" : "false";
      return;
    case Kind::kNumber:
      out += value.text;
      return;
    case Kind::kString:
      WriteString(value.text, out);
      return;
    case Kind::kArray: {
      out.push_back('[');
      for (std::size_t i = 0; i < value.items.size(); ++i) {
        if (i > 0) out.push_back(',');
        WriteValue(value.items[i], out);
      }
      out.push_back(']');
      return;
    }
    case Kind::kObject: {
      out.push_back('{');
      for (std::size_t i = 0; i < value.members.size(); ++i) {
        if (i > 0) out.push_back(',');
        WriteString(value.members[i].key, out);
        out.push_back(':');
        WriteValue(value.members[i].value, out);
      }
      out.push_back('}');
      return;
    }
  }
}

}  // namespace

Value Value::Number(double number) {
  Value value;
  value.kind = Kind::kNumber;
  // The shortest text that reads back as the same double.
  value.text = std::format("{}", number);
  return value;
}

Value Value::Float(float number) {
  Value value;
  value.kind = Kind::kNumber;
  value.text = std::format("{}", number);
  return value;
}

Value Value::Object() {
  Value value;
  value.kind = Kind::kObject;
  return value;
}

const Value* Value::Find(std::string_view key) const noexcept {
  for (const Member& member : members) {
    if (member.key == key) return &member.value;
  }
  return nullptr;
}

Value* Value::Find(std::string_view key) noexcept {
  for (Member& member : members) {
    if (member.key == key) return &member.value;
  }
  return nullptr;
}

void Value::Set(std::string_view key, Value value) {
  if (Value* existing = Find(key)) {
    *existing = std::move(value);
    return;
  }
  members.push_back(Member{std::string(key), std::move(value)});
}

std::optional<double> Value::AsDouble() const noexcept {
  if (kind != Kind::kNumber) return std::nullopt;
  double number = 0.0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto [last, error] = std::from_chars(begin, end, number);
  if (error != std::errc() || last != end) return std::nullopt;
  return number;
}

std::expected<Value, Error> Parse(std::string_view text) {
  return Parser(text).Document();
}

std::string Write(const Value& value) {
  std::string out;
  WriteValue(value, out);
  return out;
}

}  // namespace se::editor::json
