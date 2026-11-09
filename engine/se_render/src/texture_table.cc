// Copyright 2026 The simple-engine-2 Authors.

#include "se_render/texture_table.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/log.h"
#include "se_platform/fs.h"
#include "se_render/gpu_resources.h"
#include "se_render/image_io.h"

namespace se {
namespace {

// A reader for the one shape the table has: an object of string values.
class TableReader {
 public:
  TableReader(std::string_view text, std::string_view source)
      : text_(text), source_(source) {}

  std::expected<std::vector<TextureTableEntry>, Error> Read() {
    std::vector<TextureTableEntry> entries;
    SkipSpace();
    if (!Consume('{')) return Bad("expected '{'");
    SkipSpace();
    if (Consume('}')) return Finish(std::move(entries));
    while (true) {
      SkipSpace();
      auto name = String();
      if (!name) return std::unexpected(std::move(name.error()));
      for (const TextureTableEntry& entry : entries) {
        if (entry.name == *name) {
          return Bad(std::format("texture '{}' appears twice", *name));
        }
      }
      SkipSpace();
      if (!Consume(':')) return Bad("expected ':'");
      SkipSpace();
      auto path = String();
      if (!path) return std::unexpected(std::move(path.error()));
      entries.push_back({std::move(*name), std::move(*path)});
      SkipSpace();
      if (Consume('}')) return Finish(std::move(entries));
      if (!Consume(',')) return Bad("expected ',' or '}'");
    }
  }

 private:
  std::expected<std::vector<TextureTableEntry>, Error> Finish(
      std::vector<TextureTableEntry> entries) {
    SkipSpace();
    if (pos_ != text_.size()) return Bad("text after the object");
    return entries;
  }

  std::unexpected<Error> Bad(std::string message) const {
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
    return std::unexpected(Error{ErrorCode::kParse,
                                 "texture table: " + std::move(message),
                                 std::string(source_), line, column});
  }

  void SkipSpace() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
            text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool Consume(char c) {
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  std::optional<std::uint32_t> Hex4() {
    if (pos_ + 4 > text_.size()) return std::nullopt;
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return std::nullopt;
      }
    }
    return value;
  }

  static void AppendUtf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
      out += static_cast<char>(code);
    } else if (code < 0x800) {
      out += static_cast<char>(0xC0 | (code >> 6));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
      out += static_cast<char>(0xE0 | (code >> 12));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (code >> 18));
      out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }

  std::expected<std::string, Error> String() {
    if (!Consume('"')) return Bad("expected a string");
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return out;
      if (static_cast<unsigned char>(c) < 0x20) {
        return Bad("control character in a string");
      }
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= text_.size()) break;
      const char escape = text_[pos_++];
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          out += escape;
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u': {
          std::optional<std::uint32_t> code = Hex4();
          if (!code) return Bad("bad \\u escape");
          if (*code >= 0xD800 && *code < 0xDC00) {
            if (!Consume('\\') || !Consume('u')) {
              return Bad("unpaired surrogate");
            }
            const std::optional<std::uint32_t> low = Hex4();
            if (!low || *low < 0xDC00 || *low >= 0xE000) {
              return Bad("unpaired surrogate");
            }
            *code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
          } else if (*code >= 0xDC00 && *code < 0xE000) {
            return Bad("unpaired surrogate");
          }
          AppendUtf8(out, *code);
          break;
        }
        default:
          return Bad(std::format("bad escape '\\{}'", escape));
      }
    }
    return Bad("unterminated string");
  }

  std::string_view text_;
  std::string_view source_;
  std::size_t pos_ = 0;
};

}  // namespace

std::expected<std::vector<TextureTableEntry>, Error> ParseTextureTable(
    std::string_view json, std::string_view source) {
  return TableReader(json, source).Read();
}

std::expected<void, Error> LoadTextureFile(GpuResources& resources,
                                           TextureHandle handle,
                                           std::string_view path) {
  auto payload = LoadPng(path);
  if (!payload) return std::unexpected(std::move(payload.error()));
  return resources.QueueUpload(
      handle, std::make_shared<const TexturePayload>(std::move(*payload)));
}

std::expected<TextureLoadReport, Error> LoadTextureTable(
    GpuResources& resources, std::string_view content_root,
    std::string_view table_path) {
  const std::string table = fs::Join(content_root, table_path);
  auto text = fs::ReadTextFile(table);
  if (!text) {
    Error error = std::move(text.error());
    if (error.path.empty()) error.path = table;
    return std::unexpected(std::move(error));
  }
  auto entries = ParseTextureTable(*text, table);
  if (!entries) return std::unexpected(std::move(entries.error()));
  TextureLoadReport report;
  for (const TextureTableEntry& entry : *entries) {
    const TextureHandle handle = resources.AddTexture(entry.name);
    const std::string path = fs::Join(content_root, entry.path);
    report.entries.push_back({entry.name, path});
    if (auto loaded = LoadTextureFile(resources, handle, path); !loaded) {
      Error error = std::move(loaded.error());
      if (error.path.empty()) error.path = path;
      error.message =
          std::format("texture '{}': {}", entry.name, error.message);
      report.errors.push_back(std::move(error));
      continue;
    }
    ++report.loaded;
  }
  return report;
}

}  // namespace se
