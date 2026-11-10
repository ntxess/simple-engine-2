// Copyright 2026 The simple-engine-2 Authors.

#include "se_assets/atlas_page.h"

#include <lz4.h>
#include <lz4hc.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_assets/background_lane.h"
#include "se_assets/lane_jobs.h"
#include "se_assets/payloads.h"
#include "se_core/error.h"
#include "se_core/hash.h"
#include "se_platform/fs.h"

namespace se {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic = {'S', 'E', 'A', 'T',
                                                'L', 'A', 'S', 0x1A};
constexpr std::size_t kFixedHeaderBytes = 64;
constexpr std::uint32_t kFormatRgba8 = 1;
constexpr std::size_t kBlockEntryBytes = 16;
constexpr std::size_t kRegionFixedBytes = 18;

void PutU16(std::vector<std::byte>& out, std::uint16_t v) {
  for (int i = 0; i < 2; ++i) {
    out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
  }
}
void PutU32(std::vector<std::byte>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
  }
}
void PutU64(std::vector<std::byte>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
  }
}
void SetU64(std::vector<std::byte>& out, std::size_t at, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out[at + static_cast<std::size_t>(i)] =
        static_cast<std::byte>((v >> (8 * i)) & 0xFF);
  }
}

// Little-endian reads with bounds checks.
class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  bool U16(std::uint16_t& v) { return Get(2, v); }
  bool U32(std::uint32_t& v) { return Get(4, v); }
  bool U64(std::uint64_t& v) { return Get(8, v); }
  bool Bytes(std::size_t count, std::string& out) {
    if (bytes_.size() - pos_ < count) return false;
    out.assign(reinterpret_cast<const char*>(bytes_.data() + pos_), count);
    pos_ += count;
    return true;
  }
  [[nodiscard]] std::size_t Pos() const noexcept { return pos_; }
  void Seek(std::size_t pos) noexcept { pos_ = pos; }

 private:
  template <class T>
  bool Get(std::size_t count, T& v) {
    if (bytes_.size() - pos_ < count) return false;
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
      value |= std::uint64_t{std::to_integer<std::uint8_t>(bytes_[pos_ + i])}
               << (8 * i);
    }
    v = static_cast<T>(value);
    pos_ += count;
    return true;
  }

  std::span<const std::byte> bytes_;
  std::size_t pos_ = 0;
};

std::unexpected<Error> Corrupt(std::string_view path, std::string message) {
  return std::unexpected(Error{ErrorCode::kCorrupt,
                               "atlas page: " + std::move(message),
                               std::string(path), 0, 0});
}

}  // namespace

std::uint64_t SourceHash(std::span<const std::byte> bytes) {
  Hasher hasher;
  hasher.AddBytes(bytes);
  return hasher.Finish();
}

std::expected<std::vector<std::byte>, Error> EncodeAtlasPage(
    const AtlasPage& page, std::uint32_t block_bytes, int level) {
  if (page.width == 0 || page.height == 0 ||
      page.pixels.size() != page.Bytes()) {
    return Fail(ErrorCode::kInvalidArgument,
                "atlas page: empty, or pixels that do not match its size");
  }
  if (block_bytes == 0 ||
      block_bytes > static_cast<std::uint32_t>(LZ4_MAX_INPUT_SIZE)) {
    return Fail(ErrorCode::kInvalidArgument, "atlas page: bad block size");
  }
  const std::uint64_t raw_total = page.Bytes();
  const auto block_count =
      static_cast<std::uint32_t>((raw_total + block_bytes - 1) / block_bytes);

  std::vector<std::byte> out;
  out.reserve(kFixedHeaderBytes + page.pixels.size() / 2);
  for (const std::uint8_t c : kMagic) {
    out.push_back(static_cast<std::byte>(c));
  }
  PutU32(out, kAtlasVersion);
  PutU32(out, page.width);
  PutU32(out, page.height);
  PutU32(out, kFormatRgba8);
  PutU32(out, block_bytes);
  PutU32(out, block_count);
  PutU32(out, static_cast<std::uint32_t>(page.regions.size()));
  PutU32(out, 0);
  PutU64(out, page.source_size);
  PutU64(out, page.source_hash);
  const std::size_t pixel_hash_at = out.size();
  PutU64(out, 0);
  for (const AtlasRegion& region : page.regions) {
    if (region.name.size() > std::numeric_limits<std::uint16_t>::max()) {
      return Fail(ErrorCode::kInvalidArgument,
                  "atlas page: region name too long");
    }
    PutU32(out, region.x);
    PutU32(out, region.y);
    PutU32(out, region.width);
    PutU32(out, region.height);
    PutU16(out, static_cast<std::uint16_t>(region.name.size()));
    for (const char c : region.name) out.push_back(static_cast<std::byte>(c));
  }
  const std::size_t table_at = out.size();
  out.resize(out.size() + std::size_t{block_count} * kBlockEntryBytes);

  Hasher pixel_hash;
  std::vector<char> scratch(static_cast<std::size_t>(
      LZ4_compressBound(static_cast<int>(block_bytes))));
  for (std::uint32_t i = 0; i < block_count; ++i) {
    const std::uint64_t begin = std::uint64_t{i} * block_bytes;
    const auto raw = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(block_bytes, raw_total - begin));
    const auto* source =
        reinterpret_cast<const char*>(page.pixels.data() + begin);
    pixel_hash.AddBytes(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(source), raw));
    const int compressed =
        LZ4_compress_HC(source, scratch.data(), static_cast<int>(raw),
                        static_cast<int>(scratch.size()), level);
    if (compressed <= 0) {
      return Fail(ErrorCode::kIo, "atlas page: LZ4 compression failed");
    }
    const std::uint64_t offset = out.size();
    const auto* begin_out = reinterpret_cast<const std::byte*>(scratch.data());
    out.insert(out.end(), begin_out, begin_out + compressed);
    std::vector<std::byte> entry;
    PutU64(entry, offset);
    PutU32(entry, static_cast<std::uint32_t>(compressed));
    PutU32(entry, raw);
    std::memcpy(out.data() + table_at + std::size_t{i} * kBlockEntryBytes,
                entry.data(), kBlockEntryBytes);
  }
  SetU64(out, pixel_hash_at, pixel_hash.Finish());
  return out;
}

std::expected<AtlasPageDecoder, Error> AtlasPageDecoder::Begin(
    std::vector<std::byte> file, std::string_view path) {
  AtlasPageDecoder decoder;
  decoder.path_ = std::string(path);
  decoder.file_ = std::move(file);
  const std::span<const std::byte> bytes(decoder.file_);
  if (bytes.size() < kFixedHeaderBytes ||
      std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
    return Corrupt(path, "not a .seatlas file");
  }
  Reader reader(bytes);
  reader.Seek(kMagic.size());
  AtlasHeader& header = decoder.header_;
  std::uint32_t format = 0;
  std::uint32_t region_count = 0;
  std::uint32_t reserved = 0;
  if (!reader.U32(header.version) || !reader.U32(header.width) ||
      !reader.U32(header.height) || !reader.U32(format) ||
      !reader.U32(header.block_bytes) || !reader.U32(header.block_count) ||
      !reader.U32(region_count) || !reader.U32(reserved) ||
      !reader.U64(header.source_size) || !reader.U64(header.source_hash) ||
      !reader.U64(header.pixel_hash)) {
    return Corrupt(path, "truncated header");
  }
  if (header.version != kAtlasVersion) {
    return std::unexpected(
        Error{ErrorCode::kVersion,
              std::format("atlas page version {} (this build reads {})",
                          header.version, kAtlasVersion),
              std::string(path), 0, 0});
  }
  if (format != kFormatRgba8) return Corrupt(path, "unknown pixel format");
  const std::uint64_t raw_total =
      std::uint64_t{header.width} * header.height * 4;
  if (header.width == 0 || header.height == 0 || header.block_bytes == 0 ||
      header.block_count !=
          (raw_total + header.block_bytes - 1) / header.block_bytes) {
    return Corrupt(path, "size and blocks disagree");
  }
  if (std::size_t{region_count} * kRegionFixedBytes > bytes.size()) {
    return Corrupt(path, "too many regions");
  }
  for (std::uint32_t i = 0; i < region_count; ++i) {
    AtlasRegion region;
    std::uint16_t name_size = 0;
    if (!reader.U32(region.x) || !reader.U32(region.y) ||
        !reader.U32(region.width) || !reader.U32(region.height) ||
        !reader.U16(name_size) || !reader.Bytes(name_size, region.name)) {
      return Corrupt(path, "truncated region table");
    }
    if (std::uint64_t{region.x} + region.width > header.width ||
        std::uint64_t{region.y} + region.height > header.height) {
      return Corrupt(
          path, std::format("region '{}' lies outside the page", region.name));
    }
    header.regions.push_back(std::move(region));
  }
  if (std::uint64_t{header.block_count} * kBlockEntryBytes >
      bytes.size() - reader.Pos()) {
    return Corrupt(path, "truncated block table");
  }
  std::uint64_t raw_sum = 0;
  decoder.blocks_.reserve(header.block_count);
  for (std::uint32_t i = 0; i < header.block_count; ++i) {
    Block block;
    reader.U64(block.offset);
    reader.U32(block.compressed);
    reader.U32(block.raw);
    if (block.offset > bytes.size() ||
        block.compressed > bytes.size() - block.offset ||
        block.raw > header.block_bytes ||
        block.compressed > static_cast<std::uint32_t>(LZ4_MAX_INPUT_SIZE)) {
      return Corrupt(path, std::format("block {} lies outside the file", i));
    }
    raw_sum += block.raw;
    decoder.blocks_.push_back(block);
  }
  if (raw_sum != raw_total) {
    return Corrupt(path, "blocks do not cover the page");
  }
  // Reserved, not filled: each block step touches only its own bytes.
  decoder.pixels_.reserve(static_cast<std::size_t>(raw_total));
  return decoder;
}

std::expected<void, Error> AtlasPageDecoder::DecodeNextBlock() {
  if (Done()) return {};
  const Block& block = blocks_[next_];
  const std::size_t at = pixels_.size();
  pixels_.resize(at + block.raw);
  const int got = LZ4_decompress_safe(
      reinterpret_cast<const char*>(file_.data() + block.offset),
      reinterpret_cast<char*>(pixels_.data() + at),
      static_cast<int>(block.compressed), static_cast<int>(block.raw));
  if (got < 0 || static_cast<std::uint32_t>(got) != block.raw) {
    return Corrupt(path_, std::format("block {} does not decode", next_));
  }
  hash_.AddBytes(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(pixels_.data() + at), block.raw));
  ++next_;
  return {};
}

std::expected<AtlasPage, Error> AtlasPageDecoder::Finish() {
  if (!Done()) return Corrupt(path_, "Finish before the last block");
  if (hash_.Finish() != header_.pixel_hash) {
    return Corrupt(path_, "the pixels do not match the page's hash");
  }
  AtlasPage page;
  page.width = header_.width;
  page.height = header_.height;
  page.pixels = std::move(pixels_);
  page.regions = header_.regions;
  page.source_size = header_.source_size;
  page.source_hash = header_.source_hash;
  file_ = {};
  return page;
}

std::string CookedPagePath(std::string_view cooked_root,
                           std::string_view source_path) {
  if (source_path.empty() || fs::IsAbsolute(source_path)) return {};
  std::string relative = fs::Normalize(source_path);
  const std::size_t slash = relative.find_last_of('/');
  const std::size_t dot = relative.find_last_of('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    relative.resize(dot);
  }
  relative += kAtlasExtension;
  return fs::Join(cooked_root, relative);
}

// --- The lane job ---

namespace {

class AtlasJob final : public LaneJob {
 public:
  explicit AtlasJob(AtlasRequest request)
      : request_(std::move(request)),
        source_(request_.source_path),
        page_(request_.page_path) {
    using Mode = AtlasRequest::Mode;
    const bool has_source = !request_.source_path.empty();
    if (request_.mode == Mode::kCook || request_.page_path.empty()) {
      phase_ = Phase::kReadSource;
      cook_ = true;
    } else if (request_.mode == Mode::kPageIfFresh && has_source) {
      phase_ = Phase::kReadSource;
    } else {
      phase_ = Phase::kReadPage;
    }
  }

  [[nodiscard]] std::string_view Kind() const noexcept override {
    return "atlas";
  }

  StepResult Step(LaneContext& /*lane*/) override {
    switch (phase_) {
      case Phase::kReadSource:
        return ReadSource();
      case Phase::kReadPage:
        return ReadPage();
      case Phase::kHeader:
        return Header();
      case Phase::kBlocks:
        return Blocks();
      case Phase::kCook:
        return Cook();
    }
    return StepResult::Done();
  }

  std::expected<LanePayload, Error> TakeResult() override {
    if (!result_) return std::unexpected(std::move(result_.error()));
    return LanePayload(std::move(*result_));
  }

 private:
  enum class Phase : std::uint8_t {
    kReadSource,
    kReadPage,
    kHeader,
    kBlocks,
    kCook
  };

  StepResult End(std::expected<AtlasPage, Error> result) {
    result_ = std::move(result);
    return StepResult::Done();
  }

  StepResult ReadSource() {
    auto done = source_.Step();
    if (!done) {
      // Without its source a fresh-check falls back to the page alone.
      if (!cook_ && done.error().code == ErrorCode::kNotFound) {
        have_source_ = false;
        phase_ = Phase::kReadPage;
        return StepResult::Continue();
      }
      return End(std::unexpected(std::move(done.error())));
    }
    if (!*done) return StepResult::Continue();
    have_source_ = true;
    source_hash_ = SourceHash(source_.Bytes());
    phase_ = cook_ ? Phase::kCook : Phase::kReadPage;
    return StepResult::Continue();
  }

  // A page that cannot be used: cook from the source when there is one.
  StepResult PageUnusable(Error error) {
    if (have_source_) {
      phase_ = Phase::kCook;
      return StepResult::Continue();
    }
    return End(std::unexpected(std::move(error)));
  }

  StepResult ReadPage() {
    auto done = page_.Step();
    if (!done) return PageUnusable(std::move(done.error()));
    if (*done) phase_ = Phase::kHeader;
    return StepResult::Continue();
  }

  StepResult Header() {
    auto decoder =
        AtlasPageDecoder::Begin(std::move(page_.Bytes()), request_.page_path);
    if (!decoder) return PageUnusable(std::move(decoder.error()));
    if (have_source_ &&
        (decoder->Header().source_hash != source_hash_ ||
         decoder->Header().source_size != source_.Bytes().size())) {
      // The source changed since the page was cooked.
      phase_ = Phase::kCook;
      return StepResult::Continue();
    }
    decoder_.emplace(std::move(*decoder));
    phase_ = Phase::kBlocks;
    return StepResult::Continue();
  }

  StepResult Blocks() {
    if (!decoder_) {
      return End(std::unexpected(Error{ErrorCode::kCorrupt,
                                       "atlas page: no header decoded",
                                       request_.page_path, 0, 0}));
    }
    if (auto decoded = decoder_->DecodeNextBlock(); !decoded) {
      return End(std::unexpected(std::move(decoded.error())));
    }
    if (!decoder_->Done()) return StepResult::Continue();
    return End(decoder_->Finish());
  }

  StepResult Cook() {
    auto page = CookPng(source_.Bytes(), request_.name, request_.source_path);
    if (page) page->recooked = true;
    return End(std::move(page));
  }

  AtlasRequest request_;
  SteppedRead source_;
  SteppedRead page_;
  Phase phase_ = Phase::kReadPage;
  bool cook_ = false;
  bool have_source_ = false;
  std::uint64_t source_hash_ = 0;
  std::optional<AtlasPageDecoder> decoder_;
  std::expected<AtlasPage, Error> result_;
};

}  // namespace

std::unique_ptr<LaneJob> MakeAtlasJob(AtlasRequest request) {
  return std::make_unique<AtlasJob>(std::move(request));
}

}  // namespace se
