// Copyright 2026 The simple-engine-2 Authors.
//
// Cooked atlas pages (docs/milestone-specs.md M9; ADR-078): a `.seatlas`
// file is a small header, a region table and independently LZ4-compressed
// blocks of about kAtlasBlockBytes (256 KB) of RGBA8, so the background
// lane decodes a page one block per step (about 2 ms or less).
//
//   offset  size  field (little-endian)
//        0     8  magic "SEATLAS" 0x1A
//        8     4  version (1)
//       12     4  width          16  4  height
//       20     4  pixel format (1: RGBA8, straight alpha, rows top down)
//       24     4  nominal raw bytes per block
//       28     4  block count    32  4  region count   36  4  reserved (0)
//       40     8  source size    48  8  source hash (SourceHash of the
//                                         file the page was cooked from)
//       56     8  pixel hash (Hasher over the raw blocks, one AddBytes per
//                 block)
//       64        regions: x, y, width, height (4 each), name length (2),
//                 name (UTF-8)
//                 blocks: offset (8), compressed bytes (4), raw bytes (4)
//                 then the LZ4 blocks
//
// tools/asset_cook writes one page per texture of config/texture.json,
// with one region (the texture's name) covering the page; the region table
// lets M10's atlas packing put several textures on one page without a
// format change. stb_image runs only at import: in the cook tool, and on
// the lane in dev builds when a page is missing or stale, or its source
// changed (hot reload).

#ifndef SE_ASSETS_ATLAS_PAGE_H_
#define SE_ASSETS_ATLAS_PAGE_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "se_assets/background_lane.h"
#include "se_assets/payloads.h"
#include "se_core/error.h"
#include "se_core/hash.h"

namespace se {

inline constexpr std::uint32_t kAtlasVersion = 1;
inline constexpr std::uint32_t kAtlasBlockBytes = 256U * 1024U;
inline constexpr std::string_view kAtlasExtension = ".seatlas";

struct AtlasHeader {
  std::uint32_t version = kAtlasVersion;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t block_bytes = kAtlasBlockBytes;
  std::uint32_t block_count = 0;
  std::uint64_t source_size = 0;
  std::uint64_t source_hash = 0;
  std::uint64_t pixel_hash = 0;
  std::vector<AtlasRegion> regions;
};

// The hash a page records of its source file.
[[nodiscard]] std::uint64_t SourceHash(std::span<const std::byte> bytes);

// Decodes a PNG with stb_image into a page of one region, |name|, covering
// it (the cook tool; the lane's re-cook in dev builds).
[[nodiscard]] std::expected<AtlasPage, Error> CookPng(
    std::span<const std::byte> png, std::string_view name,
    std::string_view source_path);

// Encodes |page| as a .seatlas file: blocks of |block_bytes| raw bytes,
// each compressed with LZ4 HC at |level|.
[[nodiscard]] std::expected<std::vector<std::byte>, Error> EncodeAtlasPage(
    const AtlasPage& page, std::uint32_t block_bytes = kAtlasBlockBytes,
    int level = 9);

// Decodes a .seatlas file one block at a time.
class AtlasPageDecoder {
 public:
  // Checks the header and the tables of |file| (kept by the decoder).
  [[nodiscard]] static std::expected<AtlasPageDecoder, Error> Begin(
      std::vector<std::byte> file, std::string_view path);

  [[nodiscard]] const AtlasHeader& Header() const noexcept { return header_; }
  [[nodiscard]] std::uint32_t BlocksDone() const noexcept { return next_; }
  [[nodiscard]] bool Done() const noexcept {
    return next_ == header_.block_count;
  }
  // Decodes the next block. kCorrupt when it does not decode to its size.
  [[nodiscard]] std::expected<void, Error> DecodeNextBlock();
  // After the last block: checks the pixel hash and returns the page.
  [[nodiscard]] std::expected<AtlasPage, Error> Finish();

 private:
  struct Block {
    std::uint64_t offset = 0;
    std::uint32_t compressed = 0;
    std::uint32_t raw = 0;
  };

  AtlasPageDecoder() = default;

  std::string path_;
  std::vector<std::byte> file_;
  AtlasHeader header_;
  std::vector<Block> blocks_;
  std::vector<std::uint8_t> pixels_;
  Hasher hash_;
  std::uint32_t next_ = 0;
};

// The page of |source_path| (relative to the content root) under
// |cooked_root|: the same relative path with the extension replaced by
// .seatlas. Empty for an absolute source path.
[[nodiscard]] std::string CookedPagePath(std::string_view cooked_root,
                                         std::string_view source_path);

struct AtlasRequest {
  enum class Mode : std::uint8_t {
    // The cooked page only (Release builds).
    kPage,
    // The page when its recorded source hash matches the source file; a
    // missing, stale or unreadable page is re-cooked from the source (dev
    // builds). Without a source file, the page only.
    kPageIfFresh,
    // Cook from the source (hot reload of a changed source).
    kCook,
  };

  std::string page_path;
  std::string source_path;
  // The region a cook names (the texture's name).
  std::string name;
  Mode mode = Mode::kPage;
};

// Payload: AtlasPage. One step per 1 MB read, per LZ4 block, and one for a
// cook (stb_image decodes a PNG in one step: import only).
[[nodiscard]] std::unique_ptr<LaneJob> MakeAtlasJob(AtlasRequest request);

}  // namespace se

#endif  // SE_ASSETS_ATLAS_PAGE_H_
