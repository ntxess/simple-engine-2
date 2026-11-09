// Copyright 2026 The simple-engine-2 Authors.

#include "src/json_reader.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "se_core/error.h"

namespace se::json_reader {
namespace {

constexpr int kMaxDepth = 64;

class Reader {
 public:
  Reader(std::string_view text, std::string_view source)
      : text_(text), source_(source) {}

  std::expected<Node, Error> Document() {
    auto value = Value(0);
    if (!value) return value;
    SkipSpace();
    if (pos_ != text_.size()) return Bad("text after the document");
    return value;
  }

 private:
  std::expected<Node, Error> Value(int depth) {
    if (depth > kMaxDepth) return Bad("nested too deeply");
    SkipSpace();
    if (pos_ >= text_.size()) return Bad("unexpected end of the document");
    const char c = text_[pos_];
    if (c == '{') return Object(depth);
    if (c == '[') return Array(depth);
    if (c == '"') {
      auto text = String();
      if (!text) return std::unexpected(std::move(text.error()));
      Node node;
      node.kind = Kind::kString;
      node.text = std::move(*text);
      return node;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return Number();
    if (Literal("true")) return Bool(true);
    if (Literal("false")) return Bool(false);
    if (Literal("null")) return Node{};
    return Bad(std::format("unexpected character '{}'", c));
  }

  static Node Bool(bool value) {
    Node node;
    node.kind = Kind::kBool;
    node.boolean = value;
    return node;
  }

  std::expected<Node, Error> Object(int depth) {
    ++pos_;  // '{'
    Node node;
    node.kind = Kind::kObject;
    SkipSpace();
    if (Consume('}')) return node;
    while (true) {
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        return Bad("expected a member name");
      }
      auto key = String();
      if (!key) return std::unexpected(std::move(key.error()));
      if (node.Find(*key) != nullptr) {
        return Bad(std::format("duplicate key '{}'", *key));
      }
      SkipSpace();
      if (!Consume(':')) return Bad("expected ':'");
      auto value = Value(depth + 1);
      if (!value) return value;
      node.members.emplace_back(std::move(*key), std::move(*value));
      SkipSpace();
      if (Consume('}')) return node;
      if (!Consume(',')) return Bad("expected ',' or '}'");
    }
  }

  std::expected<Node, Error> Array(int depth) {
    ++pos_;  // '['
    Node node;
    node.kind = Kind::kArray;
    SkipSpace();
    if (Consume(']')) return node;
    while (true) {
      auto value = Value(depth + 1);
      if (!value) return value;
      node.items.push_back(std::move(*value));
      SkipSpace();
      if (Consume(']')) return node;
      if (!Consume(',')) return Bad("expected ',' or ']'");
    }
  }

  std::expected<Node, Error> Number() {
    const std::size_t start = pos_;
    if (text_[pos_] == '-') ++pos_;
    while (pos_ < text_.size() &&
           ((text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '.' ||
            text_[pos_] == 'e' || text_[pos_] == 'E' || text_[pos_] == '+' ||
            text_[pos_] == '-')) {
      ++pos_;
    }
    const std::string_view digits = text_.substr(start, pos_ - start);
    Node node;
    node.kind = Kind::kNumber;
    const char* first = digits.data();
    const char* last = first + digits.size();
    const auto [ptr, ec] = std::from_chars(first, last, node.number);
    if (ec != std::errc() || ptr != last) {
      pos_ = start;
      return Bad(std::format("bad number '{}'", digits));
    }
    return node;
  }

  std::expected<std::string, Error> String() {
    ++pos_;  // '"'
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return out;
      if (static_cast<unsigned char>(c) < 0x20) {
        return Bad("control character in a string");
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) break;
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
          auto code = Hex4();
          if (!code) return std::unexpected(std::move(code.error()));
          std::uint32_t point = *code;
          if (point >= 0xD800 && point <= 0xDBFF) {
            if (!Literal("\\u")) return Bad("unpaired surrogate");
            auto low = Hex4();
            if (!low) return std::unexpected(std::move(low.error()));
            if (*low < 0xDC00 || *low > 0xDFFF) {
              return Bad("unpaired surrogate");
            }
            point = 0x10000 + ((point - 0xD800) << 10) + (*low - 0xDC00);
          } else if (point >= 0xDC00 && point <= 0xDFFF) {
            return Bad("unpaired surrogate");
          }
          AppendUtf8(point, out);
          break;
        }
        default:
          return Bad(std::format("bad escape '\\{}'", e));
      }
    }
    return Bad("unterminated string");
  }

  std::expected<std::uint32_t, Error> Hex4() {
    if (pos_ + 4 > text_.size()) return Bad("short \\u escape");
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = text_[pos_++];
      value <<= 4;
      if (h >= '0' && h <= '9') {
        value |= static_cast<std::uint32_t>(h - '0');
      } else if (h >= 'a' && h <= 'f') {
        value |= static_cast<std::uint32_t>(h - 'a' + 10);
      } else if (h >= 'A' && h <= 'F') {
        value |= static_cast<std::uint32_t>(h - 'A' + 10);
      } else {
        return Bad("bad \\u escape");
      }
    }
    return value;
  }

  static void AppendUtf8(std::uint32_t point, std::string& out) {
    if (point < 0x80) {
      out.push_back(static_cast<char>(point));
    } else if (point < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (point >> 6)));
      out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else if (point < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (point >> 12)));
      out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (point >> 18)));
      out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    }
  }

  bool Literal(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) return false;
    pos_ += word.size();
    return true;
  }

  bool Consume(char c) {
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  void SkipSpace() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
            text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  [[nodiscard]] std::unexpected<Error> Bad(std::string message) const {
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
    return std::unexpected(Error{ErrorCode::kParse, std::move(message),
                                 std::string(source_), line, column});
  }

  std::string_view text_;
  std::string_view source_;
  std::size_t pos_ = 0;
};

}  // namespace

const Node* Node::Find(std::string_view key) const noexcept {
  for (const auto& [name, value] : members) {
    if (name == key) return &value;
  }
  return nullptr;
}

std::expected<Node, Error> Parse(std::string_view text,
                                 std::string_view source) {
  // A UTF-8 byte order mark is allowed and skipped.
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  return Reader(text, source).Document();
}

}  // namespace se::json_reader
