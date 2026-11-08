// Copyright 2026 The simple-engine-2 Authors.

#include "game_rampage/json.h"

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

namespace se::rampage::json {
namespace {

// Nesting deeper than this is rejected rather than recursed into.
constexpr int kMaxDepth = 256;

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  std::expected<Value, Error> Document() {
    SkipSpace();
    Value value;
    if (!ParseValue(value, 0)) return std::unexpected(error_);
    SkipSpace();
    if (pos_ != text_.size()) {
      Fail("unexpected text after the document");
      return std::unexpected(error_);
    }
    return value;
  }

 private:
  bool Fail(std::string_view message) {
    if (failed_) return false;
    failed_ = true;
    std::int32_t line = 1;
    std::int32_t column = 1;
    for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
      if (text_[i] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
    }
    error_ = Error{ErrorCode::kParse,
                   std::format("{}:{}: {}", line, column, message),
                   {},
                   line,
                   column};
    return false;
  }

  void SkipSpace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
      ++pos_;
    }
  }

  bool Literal(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) {
      return Fail("invalid literal");
    }
    pos_ += word.size();
    return true;
  }

  bool ParseValue(Value& out, int depth) {
    if (depth > kMaxDepth) return Fail("nesting too deep");
    if (pos_ >= text_.size()) return Fail("unexpected end of input");
    const char c = text_[pos_];
    switch (c) {
      case '{':
        return ParseObject(out, depth);
      case '[':
        return ParseArray(out, depth);
      case '"': {
        std::string s;
        if (!ParseString(s)) return false;
        out = Value::String(std::move(s));
        return true;
      }
      case 't':
        if (!Literal("true")) return false;
        out = Value::Bool(true);
        return true;
      case 'f':
        if (!Literal("false")) return false;
        out = Value::Bool(false);
        return true;
      case 'n':
        if (!Literal("null")) return false;
        out = Value();
        return true;
      default:
        return ParseNumber(out);
    }
  }

  bool Digits() {
    const std::size_t start = pos_;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      ++pos_;
    }
    return pos_ > start;
  }

  bool ParseNumber(Value& out) {
    const std::size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    if (pos_ < text_.size() && text_[pos_] == '0') {
      ++pos_;
    } else if (!Digits()) {
      return Fail("expected a value");
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (!Digits()) return Fail("expected digits after '.'");
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      if (!Digits()) return Fail("expected digits in the exponent");
    }
    out = Value::Number(std::string(text_.substr(start, pos_ - start)));
    return true;
  }

  bool Hex4(std::uint32_t& out) {
    if (pos_ + 4 > text_.size()) return Fail("truncated \\u escape");
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return Fail("invalid \\u escape");
      }
      out = (out << 4) | digit;
    }
    return true;
  }

  static void AppendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseString(std::string& out) {
    ++pos_;  // the opening quote
    while (true) {
      if (pos_ >= text_.size()) return Fail("unterminated string");
      const char c = text_[pos_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) {
        return Fail("control character in a string");
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) return Fail("unterminated escape");
      const char e = text_[pos_++];
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out.push_back(e);
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
          std::uint32_t cp = 0;
          if (!Hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            std::uint32_t low = 0;
            if (text_.substr(pos_, 2) != "\\u") {
              return Fail("unpaired surrogate");
            }
            pos_ += 2;
            if (!Hex4(low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) {
              return Fail("unpaired surrogate");
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return Fail("unpaired surrogate");
          }
          AppendUtf8(out, cp);
          break;
        }
        default:
          return Fail("invalid escape");
      }
    }
  }

  bool ParseArray(Value& out, int depth) {
    ++pos_;
    out = Value::Array();
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipSpace();
      Value item;
      if (!ParseValue(item, depth + 1)) return false;
      out.Push(std::move(item));
      SkipSpace();
      if (pos_ >= text_.size()) return Fail("unterminated array");
      const char c = text_[pos_++];
      if (c == ']') return true;
      if (c != ',') return Fail("expected ',' or ']'");
    }
  }

  bool ParseObject(Value& out, int depth) {
    ++pos_;
    out = Value::Object();
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        return Fail("expected a member name");
      }
      std::string key;
      if (!ParseString(key)) return false;
      if (out.Find(key) != nullptr) {
        return Fail(std::format("duplicate member \"{}\"", key));
      }
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != ':') {
        return Fail("expected ':'");
      }
      ++pos_;
      SkipSpace();
      Value value;
      if (!ParseValue(value, depth + 1)) return false;
      out.MutableMembers().push_back(Member{std::move(key), std::move(value)});
      SkipSpace();
      if (pos_ >= text_.size()) return Fail("unterminated object");
      const char c = text_[pos_++];
      if (c == '}') return true;
      if (c != ',') return Fail("expected ',' or '}'");
    }
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  bool failed_ = false;
  Error error_;
};

void WriteString(std::string& out, std::string_view s) {
  out.push_back('"');
  for (const char c : s) {
    switch (c) {
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
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

void Newline(std::string& out, int indent, int depth) {
  if (indent <= 0) return;
  out.push_back('\n');
  out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth),
             ' ');
}

void WriteValue(std::string& out, const Value& value, int indent, int depth) {
  switch (value.GetKind()) {
    case Kind::kNull:
      out += "null";
      return;
    case Kind::kBool:
      out += value.AsBool() ? "true" : "false";
      return;
    case Kind::kNumber:
      out += value.Text();
      return;
    case Kind::kString:
      WriteString(out, value.Text());
      return;
    case Kind::kArray: {
      out.push_back('[');
      bool first = true;
      for (const Value& item : value.Items()) {
        if (!first) out.push_back(',');
        first = false;
        Newline(out, indent, depth + 1);
        WriteValue(out, item, indent, depth + 1);
      }
      if (!value.Items().empty()) Newline(out, indent, depth);
      out.push_back(']');
      return;
    }
    case Kind::kObject: {
      out.push_back('{');
      bool first = true;
      for (const Member& member : value.Members()) {
        if (!first) out.push_back(',');
        first = false;
        Newline(out, indent, depth + 1);
        WriteString(out, member.key);
        out += indent > 0 ? ": " : ":";
        WriteValue(out, member.value, indent, depth + 1);
      }
      if (!value.Members().empty()) Newline(out, indent, depth);
      out.push_back('}');
      return;
    }
  }
}

}  // namespace

Value Value::Bool(bool value) {
  Value v;
  v.kind_ = Kind::kBool;
  v.bool_ = value;
  return v;
}

Value Value::Number(std::string text) {
  Value v;
  v.kind_ = Kind::kNumber;
  v.text_ = std::move(text);
  return v;
}

Value Value::NumberOf(double value) {
  // The shortest text that reads back as the same double.
  return Number(std::format("{}", value));
}

Value Value::NumberOf(std::int64_t value) {
  return Number(std::format("{}", value));
}

Value Value::String(std::string text) {
  Value v;
  v.kind_ = Kind::kString;
  v.text_ = std::move(text);
  return v;
}

Value Value::Array() {
  Value v;
  v.kind_ = Kind::kArray;
  return v;
}

Value Value::Object() {
  Value v;
  v.kind_ = Kind::kObject;
  return v;
}

std::optional<double> Value::AsDouble() const noexcept {
  if (kind_ != Kind::kNumber) return std::nullopt;
  double value = 0.0;
  const char* begin = text_.data();
  const char* end = begin + text_.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc() || ptr != end) return std::nullopt;
  return value;
}

std::optional<std::int64_t> Value::AsInt64() const noexcept {
  if (kind_ != Kind::kNumber) return std::nullopt;
  std::int64_t value = 0;
  const char* begin = text_.data();
  const char* end = begin + text_.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc() || ptr != end) return std::nullopt;
  return value;
}

const Value* Value::Find(std::string_view key) const noexcept {
  if (kind_ != Kind::kObject) return nullptr;
  for (const Member& member : members_) {
    if (member.key == key) return &member.value;
  }
  return nullptr;
}

Value* Value::Find(std::string_view key) noexcept {
  if (kind_ != Kind::kObject) return nullptr;
  for (Member& member : members_) {
    if (member.key == key) return &member.value;
  }
  return nullptr;
}

void Value::Set(std::string_view key, Value value) {
  if (kind_ == Kind::kNull) kind_ = Kind::kObject;
  if (Value* existing = Find(key)) {
    *existing = std::move(value);
    return;
  }
  members_.push_back(Member{std::string(key), std::move(value)});
}

bool Value::Erase(std::string_view key) {
  for (auto it = members_.begin(); it != members_.end(); ++it) {
    if (it->key == key) {
      members_.erase(it);
      return true;
    }
  }
  return false;
}

void Value::Push(Value value) {
  if (kind_ == Kind::kNull) kind_ = Kind::kArray;
  items_.push_back(std::move(value));
}

std::expected<Value, Error> Parse(std::string_view text) {
  Parser parser(text);
  return parser.Document();
}

std::string Write(const Value& value, int indent) {
  std::string out;
  WriteValue(out, value, indent, 0);
  return out;
}

void MergePatch(Value& target, const Value& patch) {
  if (!patch.IsObject()) {
    target = patch;
    return;
  }
  if (!target.IsObject()) target = Value::Object();
  for (const Member& member : patch.Members()) {
    if (member.value.IsNull()) {
      target.Erase(member.key);
      continue;
    }
    Value* existing = target.Find(member.key);
    if (existing == nullptr) {
      target.Set(member.key, Value());
      existing = target.Find(member.key);
    }
    MergePatch(*existing, member.value);
  }
}

}  // namespace se::rampage::json
