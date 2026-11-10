// Copyright 2026 The simple-engine-2 Authors.
//
// asset_cook: turns the textures of a texture table (config/texture.json)
// into cooked atlas pages (docs/milestone-specs.md M9; se_assets/
// atlas_page.h): one .seatlas per texture, a header and independent LZ4
// blocks of about 256 KB of RGBA8, at <out>/<the PNG's path relative to
// the content root, with .seatlas for its extension>. CMake runs it at
// build time into the build tree (<build>/bin/cooked); the runtime loads
// the pages through the background lane. stb_image runs here, at import.
//
//   asset_cook --content-root DIR --table FILE --out DIR [--stamp FILE]
//              [--block-bytes N] [--force]
//
// A page whose recorded source hash and size match its PNG, and whose
// version is this build's, is left as it is (--force cooks every page).
// Pages are written to a temporary file and renamed into place. --stamp
// writes the list of pages, for the build's dependency tracking. Exit 0
// when every texture cooked or was fresh, 1 otherwise (each failure names
// its file).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_assets/atlas_page.h"
#include "se_assets/payloads.h"
#include "se_core/error.h"
#include "se_platform/fs.h"
#include "se_render/texture_table.h"

namespace {

struct Args {
  std::string content_root;
  std::string table;
  std::string out;
  std::string stamp;
  std::uint32_t block_bytes = se::kAtlasBlockBytes;
  bool force = false;
};

void Print(const std::string& text) { std::fputs(text.c_str(), stdout); }
void PrintError(const std::string& text) { std::fputs(text.c_str(), stderr); }

std::expected<Args, std::string> ParseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    auto value = [&]() -> std::expected<std::string, std::string> {
      if (i + 1 >= argc) {
        return std::unexpected(std::format("{} needs a value", flag));
      }
      return std::string(argv[++i]);
    };
    if (flag == "--force") {
      args.force = true;
      continue;
    }
    auto text = value();
    if (!text) return std::unexpected(text.error());
    if (flag == "--content-root") {
      args.content_root = *text;
    } else if (flag == "--table") {
      args.table = *text;
    } else if (flag == "--out") {
      args.out = *text;
    } else if (flag == "--stamp") {
      args.stamp = *text;
    } else if (flag == "--block-bytes") {
      args.block_bytes =
          static_cast<std::uint32_t>(std::strtoul(text->c_str(), nullptr, 10));
      if (args.block_bytes == 0) {
        return std::unexpected("--block-bytes must be > 0");
      }
    } else {
      return std::unexpected(std::format("unknown option '{}'", flag));
    }
  }
  if (args.content_root.empty() || args.table.empty() || args.out.empty()) {
    return std::unexpected(
        "usage: asset_cook --content-root DIR --table FILE --out DIR "
        "[--stamp FILE] [--block-bytes N] [--force]");
  }
  return args;
}

std::span<const std::byte> AsBytes(const std::string& text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// True when |page_path| holds a page cooked from |png| by this format.
bool Fresh(const std::string& page_path, std::span<const std::byte> png) {
  auto bytes = se::fs::ReadFile(page_path);
  if (!bytes) return false;
  auto decoder = se::AtlasPageDecoder::Begin(std::move(*bytes), page_path);
  return decoder && decoder->Header().source_size == png.size() &&
         decoder->Header().source_hash == se::SourceHash(png);
}

std::string Describe(const se::Error& error) {
  return error.path.empty() ? error.message
                            : std::format("{} ({})", error.message, error.path);
}

int Run(int argc, char** argv) {
  auto args = ParseArgs(argc, argv);
  if (!args) {
    PrintError("asset_cook: " + args.error() + "\n");
    return 2;
  }
  const std::string table_path = se::fs::Join(args->content_root, args->table);
  auto table_text = se::fs::ReadTextFile(table_path);
  if (!table_text) {
    PrintError("asset_cook: " + Describe(table_text.error()) + "\n");
    return 1;
  }
  auto entries = se::ParseTextureTable(*table_text, table_path);
  if (!entries) {
    const se::Error& error = entries.error();
    PrintError(std::format("asset_cook: {}:{}:{}: {}\n", error.path, error.line,
                           error.column, error.message));
    return 1;
  }
  int cooked = 0;
  int fresh = 0;
  int failed = 0;
  std::string stamp;
  for (const se::TextureTableEntry& entry : *entries) {
    const std::string source = se::fs::Join(args->content_root, entry.path);
    const std::string page = se::CookedPagePath(args->out, entry.path);
    if (page.empty()) {
      PrintError(std::format(
          "asset_cook: texture '{}': {} is not relative to the content root\n",
          entry.name, entry.path));
      ++failed;
      continue;
    }
    auto png = se::fs::ReadFile(source);
    if (!png) {
      PrintError(std::format("asset_cook: texture '{}': {}\n", entry.name,
                             Describe(png.error())));
      ++failed;
      continue;
    }
    stamp += page + "\n";
    if (!args->force && Fresh(page, *png)) {
      ++fresh;
      continue;
    }
    auto atlas = se::CookPng(*png, entry.name, source);
    if (!atlas) {
      PrintError(std::format("asset_cook: texture '{}': {}\n", entry.name,
                             Describe(atlas.error())));
      ++failed;
      continue;
    }
    auto encoded = se::EncodeAtlasPage(*atlas, args->block_bytes);
    if (!encoded) {
      PrintError(std::format("asset_cook: texture '{}': {}\n", entry.name,
                             Describe(encoded.error())));
      ++failed;
      continue;
    }
    if (auto made = se::fs::CreateDirectories(se::fs::Parent(page)); !made) {
      PrintError("asset_cook: " + Describe(made.error()) + "\n");
      ++failed;
      continue;
    }
    if (auto written = se::fs::WriteFileAtomic(page, *encoded); !written) {
      PrintError("asset_cook: " + Describe(written.error()) + "\n");
      ++failed;
      continue;
    }
    Print(std::format("asset_cook: {} -> {} ({}x{}, {} bytes, {:.0f}%)\n",
                      entry.path, page, atlas->width, atlas->height,
                      encoded->size(),
                      100.0 * static_cast<double>(encoded->size()) /
                          static_cast<double>(atlas->Bytes())));
    ++cooked;
  }
  if (!args->stamp.empty() && failed == 0) {
    if (auto written = se::fs::WriteFileAtomic(args->stamp, AsBytes(stamp));
        !written) {
      PrintError("asset_cook: " + Describe(written.error()) + "\n");
      return 1;
    }
  }
  Print(std::format("asset_cook: {} textures: {} cooked, {} fresh, {} failed\n",
                    entries->size(), cooked, fresh, failed));
  return failed == 0 ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
  // A tool may catch (ADR-006 exempts test and tool code).
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    PrintError(std::string("asset_cook: ") + error.what() + "\n");
    return 1;
  }
}
