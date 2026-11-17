// Copyright 2026 The simple-engine-2 Authors.

#include "se_sim/content_bundle.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_sim/session_log.h"

namespace se {
namespace {

constexpr std::uint64_t kPathGuidSeed = 0xC0A7E47B0D1E5EEDULL;
// JSON's brackets by name: cpplint's brace matching reads the characters
// in a condition as code.
constexpr char kObjectOpen = 0x7B;
constexpr char kObjectClose = 0x7D;
constexpr char kArrayOpen = 0x5B;
constexpr char kArrayClose = 0x5D;
constexpr int kHexDigits = 16;

void AppendEscaped(std::string& out, std::string_view text) {
  out += '"';
  for (const char c : text) {
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
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

// A reader for the bundle's JSON: objects, arrays, strings (with every
// escape), and other values skipped. Errors carry the line and column.
class JsonCursor {
 public:
  explicit JsonCursor(std::string_view text) : text_(text) {}

  [[nodiscard]] std::unexpected<Error> Bad(std::string_view what) const {
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
                                 "content bundle: " + std::string(what),
                                 {},
                                 line,
                                 column});
  }

  void SkipSpace() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' ||
            text_[pos_] == '\t')) {
      ++pos_;
    }
  }

  // Consumes |c| after spaces.
  bool Take(char c) {
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  [[nodiscard]] bool Peek(char c) {
    SkipSpace();
    return pos_ < text_.size() && text_[pos_] == c;
  }

  [[nodiscard]] bool AtEnd() {
    SkipSpace();
    return pos_ >= text_.size();
  }

  std::expected<std::string, Error> String() {
    if (!Take('"')) return Bad("expected a string");
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return out;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= text_.size()) break;
      const char e = text_[pos_++];
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out += e;
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
          auto code = Hex4();
          if (!code) return std::unexpected(code.error());
          std::uint32_t point = *code;
          if (point >= 0xD800 && point < 0xDC00) {
            if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' ||
                text_[pos_ + 1] != 'u') {
              return Bad("an unpaired surrogate");
            }
            pos_ += 2;
            auto low = Hex4();
            if (!low) return std::unexpected(low.error());
            if (*low < 0xDC00 || *low >= 0xE000) {
              return Bad("an unpaired surrogate");
            }
            point = 0x10000 + ((point - 0xD800) << 10) + (*low - 0xDC00);
          }
          AppendUtf8(out, point);
          break;
        }
        default:
          return Bad("an unknown escape");
      }
    }
    return Bad("an unterminated string");
  }

  // Skips any value.
  std::expected<void, Error> Skip() {
    SkipSpace();
    if (pos_ >= text_.size()) return Bad("expected a value");
    const char c = text_[pos_];
    if (c == '"') {
      auto skipped = String();
      if (!skipped) return std::unexpected(skipped.error());
      return {};
    }
    if (c == kObjectOpen || c == kArrayOpen) {
      const bool object = c == kObjectOpen;
      const char close = object ? kObjectClose : kArrayClose;
      ++pos_;
      if (Take(close)) return {};
      do {
        if (object) {
          auto key = String();
          if (!key) return std::unexpected(key.error());
          if (!Take(':')) return Bad("expected ':'");
        }
        auto value = Skip();
        if (!value) return value;
      } while (Take(','));
      if (!Take(close)) return Bad("expected ',' or the end of a container");
      return {};
    }
    // A number, true, false or null.
    const std::size_t start = pos_;
    while (pos_ < text_.size() && text_[pos_] != ',' && text_[pos_] != '}' &&
           text_[pos_] != ']' && text_[pos_] != ' ' && text_[pos_] != '\n' &&
           text_[pos_] != '\r' && text_[pos_] != '\t') {
      ++pos_;
    }
    if (pos_ == start) return Bad("expected a value");
    return {};
  }

 private:
  std::expected<std::uint32_t, Error> Hex4() {
    if (pos_ + 4 > text_.size()) return Bad("a short \\u escape");
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
        return Bad("a bad \\u escape");
      }
    }
    return value;
  }

  static void AppendUtf8(std::string& out, std::uint32_t point) {
    if (point < 0x80) {
      out += static_cast<char>(point);
    } else if (point < 0x800) {
      out += static_cast<char>(0xC0 | (point >> 6));
      out += static_cast<char>(0x80 | (point & 0x3F));
    } else if (point < 0x10000) {
      out += static_cast<char>(0xE0 | (point >> 12));
      out += static_cast<char>(0x80 | ((point >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (point & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (point >> 18));
      out += static_cast<char>(0x80 | ((point >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((point >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (point & 0x3F));
    }
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

std::expected<std::uint64_t, Error> ParseHash(std::string_view hex) {
  if (hex.size() != kHexDigits) {
    return Fail(ErrorCode::kParse, "content bundle: a hash is 16 hex digits");
  }
  std::uint64_t value = 0;
  for (const char h : hex) {
    value <<= 4;
    if (h >= '0' && h <= '9') {
      value |= static_cast<std::uint64_t>(h - '0');
    } else if (h >= 'a' && h <= 'f') {
      value |= static_cast<std::uint64_t>(h - 'a' + 10);
    } else {
      return Fail(ErrorCode::kParse,
                  "content bundle: a hash is lower-case hex digits");
    }
  }
  return value;
}

std::expected<ContentText, Error> ReadText(JsonCursor& in) {
  ContentText text;
  bool has_path = false;
  bool has_hash = false;
  bool has_text = false;
  std::string hash;
  if (!in.Take('{')) return in.Bad("expected a text object");
  if (!in.Take('}')) {
    do {
      auto key = in.String();
      if (!key) return std::unexpected(key.error());
      if (!in.Take(':')) return in.Bad("expected ':'");
      if (*key == "path" || *key == "hash" || *key == "text") {
        auto value = in.String();
        if (!value) return std::unexpected(value.error());
        if (*key == "path") {
          text.path = std::move(*value);
          has_path = true;
        } else if (*key == "hash") {
          hash = std::move(*value);
          has_hash = true;
        } else {
          text.text = std::move(*value);
          has_text = true;
        }
      } else if (auto skipped = in.Skip(); !skipped) {
        return std::unexpected(skipped.error());
      }
    } while (in.Take(','));
    if (!in.Take('}')) return in.Bad("expected ',' or '}'");
  }
  if (!has_path || !has_hash || !has_text) {
    return in.Bad("a text needs \"path\", \"hash\" and \"text\"");
  }
  auto parsed = ParseHash(hash);
  if (!parsed) return std::unexpected(parsed.error());
  text.hash = *parsed;
  if (ContentTextHash(text.text) != text.hash) {
    return Fail(ErrorCode::kCorrupt, "content bundle: the text of " +
                                         text.path +
                                         " does not match its hash");
  }
  return text;
}

}  // namespace

std::uint64_t ContentTextHash(std::string_view text) noexcept {
  return ContentHash(std::as_bytes(std::span<const char>(text)));
}

Guid ContentPathGuid(std::string_view path) noexcept {
  Hasher hi(kPathGuidSeed);
  hi.AddString(path);
  Hasher lo(~kPathGuidSeed);
  lo.AddString(path);
  return Guid{hi.Finish(), lo.Finish()};
}

bool ContentBundle::Add(std::string_view path, std::string_view text) {
  const std::uint64_t hash = ContentTextHash(text);
  const bool known = std::ranges::any_of(texts_, [&](const ContentText& t) {
    return t.hash == hash && t.path == path;
  });
  if (known) return false;
  texts_.push_back(ContentText{std::string(path), hash, std::string(text)});
  return true;
}

const ContentText* ContentBundle::Latest(std::string_view path) const {
  const auto newest_first = texts_ | std::views::reverse;
  const auto newest = std::ranges::find_if(
      newest_first,
      [path](const ContentText& text) { return text.path == path; });
  return newest == newest_first.end() ? nullptr : &*newest;
}

std::vector<std::byte> ContentBundle::Write() const {
  std::string out = "{\"format\": ";
  AppendEscaped(out, kContentBundleFormat);
  out += ", \"start_state\": ";
  AppendEscaped(out, start_state_);
  out += ", \"texts\": [";
  for (std::size_t i = 0; i < texts_.size(); ++i) {
    const ContentText& text = texts_[i];
    out += i == 0 ? "\n  {\"path\": " : ",\n  {\"path\": ";
    AppendEscaped(out, text.path);
    out += std::format(", \"hash\": \"{:016x}\", \"text\": ", text.hash);
    AppendEscaped(out, text.text);
    out += "}";
  }
  out += texts_.empty() ? "]}\n" : "\n]}\n";
  const auto bytes = std::as_bytes(std::span<const char>(out));
  return {bytes.begin(), bytes.end()};
}

bool ContentBundle::IsBundle(std::span<const std::byte> bytes) {
  // The writer puts the format first; a reader of either kind of start
  // bytes needs no more than that.
  const std::string_view text(reinterpret_cast<const char*>(bytes.data()),
                              std::min<std::size_t>(bytes.size(), 128));
  JsonCursor in(text);
  if (!in.Take('{')) return false;
  auto key = in.String();
  if (!key || *key != "format" || !in.Take(':')) return false;
  auto format = in.String();
  return format && *format == kContentBundleFormat;
}

std::expected<ContentBundle, Error> ContentBundle::Read(
    std::span<const std::byte> bytes) {
  const std::string_view text(reinterpret_cast<const char*>(bytes.data()),
                              bytes.size());
  JsonCursor in(text);
  ContentBundle bundle;
  bool has_format = false;
  if (!in.Take('{')) return in.Bad("expected an object");
  if (!in.Take('}')) {
    do {
      auto key = in.String();
      if (!key) return std::unexpected(key.error());
      if (!in.Take(':')) return in.Bad("expected ':'");
      if (*key == "format") {
        auto format = in.String();
        if (!format) return std::unexpected(format.error());
        if (*format != kContentBundleFormat) {
          return Fail(ErrorCode::kVersion,
                      "content bundle: format '" + *format + "', expected " +
                          std::string(kContentBundleFormat));
        }
        has_format = true;
      } else if (*key == "start_state") {
        auto state = in.String();
        if (!state) return std::unexpected(state.error());
        bundle.start_state_ = std::move(*state);
      } else if (*key == "texts") {
        if (!in.Take('[')) return in.Bad("\"texts\" is an array");
        if (!in.Take(']')) {
          do {
            auto entry = ReadText(in);
            if (!entry) return std::unexpected(entry.error());
            bundle.texts_.push_back(std::move(*entry));
          } while (in.Take(','));
          if (!in.Take(']')) return in.Bad("expected ',' or ']'");
        }
      } else if (auto skipped = in.Skip(); !skipped) {
        return std::unexpected(skipped.error());
      }
    } while (in.Take(','));
    if (!in.Take('}')) return in.Bad("expected ',' or '}'");
  }
  if (!in.AtEnd()) return in.Bad("trailing text after the bundle");
  if (!has_format) return in.Bad("no \"format\"");
  return bundle;
}

ContentBundleSource::ContentBundleSource(ContentBundle bundle)
    : bundle_(std::move(bundle)), selected_(bundle_.Texts().size(), false) {
  // Each path starts at its first version.
  const std::span<const ContentText> texts = bundle_.Texts();
  for (std::size_t i = 0; i < texts.size(); ++i) {
    const bool earlier = std::any_of(
        texts.begin(), texts.begin() + static_cast<std::ptrdiff_t>(i),
        [&](const ContentText& t) { return t.path == texts[i].path; });
    selected_[i] = !earlier;
  }
}

std::expected<std::string_view, Error> ContentBundleSource::Text(
    std::string_view path) const {
  const std::span<const ContentText> texts = bundle_.Texts();
  for (std::size_t i = 0; i < texts.size(); ++i) {
    if (selected_[i] && texts[i].path == path) {
      return std::string_view(texts[i].text);
    }
  }
  return std::unexpected(Error{ErrorCode::kNotFound,
                               "the content bundle has no text for this path",
                               std::string(path), 0, 0});
}

bool ContentBundleSource::Select(Guid path_guid, std::uint64_t hash) {
  const std::span<const ContentText> texts = bundle_.Texts();
  const auto chosen = std::ranges::find_if(texts, [&](const ContentText& t) {
    return t.hash == hash && ContentPathGuid(t.path) == path_guid;
  });
  if (chosen == texts.end()) {
    ++unknown_swaps_;
    return false;
  }
  for (std::size_t i = 0; i < texts.size(); ++i) {
    if (texts[i].path == chosen->path) selected_[i] = &texts[i] == &*chosen;
  }
  ++selections_;
  return true;
}

void ContentBundleSource::SelectDue(const SessionLog& log, Tick next_tick) {
  std::vector<const SessionEntry*> due;
  const auto consider = [&](const SessionEntry& entry) {
    if (entry.op != SessionOp::kAssetSwap || entry.apply_before > next_tick) {
      return;
    }
    const bool taken =
        any_swap_ &&
        (entry.apply_before < swap_tick_ ||
         (entry.apply_before == swap_tick_ && entry.seq <= swap_seq_));
    if (!taken) due.push_back(&entry);
  };
  for (const SessionEntry& entry : log.Applied()) consider(entry);
  for (const SessionEntry& entry : log.Pending()) consider(entry);
  std::ranges::sort(due, [](const SessionEntry* a, const SessionEntry* b) {
    return a->apply_before != b->apply_before
               ? a->apply_before < b->apply_before
               : a->seq < b->seq;
  });
  for (const SessionEntry* entry : due) {
    std::uint64_t hash = 0;
    // The payload is the content hash as a little-endian u64
    // (MakeAssetSwap).
    if (entry->payload.size() == sizeof(hash)) {
      for (std::size_t i = 0; i < sizeof(hash); ++i) {
        hash |= std::to_integer<std::uint64_t>(entry->payload[i]) << (8 * i);
      }
      Select(entry->target, hash);
    }
    any_swap_ = true;
    swap_tick_ = entry->apply_before;
    swap_seq_ = entry->seq;
  }
}

bool ContentBundleSource::Ready(std::string_view /*state_id*/) {
  if (log_ != nullptr && driver_ != nullptr) {
    SelectDue(*log_, driver_->NextTick());
  }
  return true;
}

}  // namespace se
