// Copyright 2026 The simple-engine-2 Authors.
//
// bench.lane_steps (docs/milestone-specs.md M9): the background lane's
// bounded steps, timed one at a time with the functions the lane runs:
//
//   read_1mb      fs::FileReader::Read of 1 MB (kReadStepBytes) from a
//                 64 MB file the OS has cached (a fresh reader per pass;
//                 smoke runs use a smaller file)
//   lz4_block     AtlasPageDecoder::DecodeNextBlock of a 256 KB block
//                 (kAtlasBlockBytes) of a 64-block page, its hash included
//   scene_chunk   ParseSceneChunk of a 256-entity chunk (glaze's generic
//                 parse plus each entity written back as compact JSON)
//
// Each kind runs --warmup untimed steps, then --iterations timed ones, and
// reports the median, p95, p99 and max; --max-p99-ms gates every kind's
// p99 (the design's 2 ms estimate becomes a measured threshold). It lives
// apart from se_bench, which may not link SDL or LZ4: the reads go through
// se::fs (SDL's file API on Linux, Win32 on Windows).
//
//   se_lane_bench [--iterations K] [--warmup W] [--max-p99-ms MS]
//                 [--json FILE] [--scratch DIR]

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_assets/atlas_page.h"
#include "se_assets/lane_jobs.h"
#include "se_assets/payloads.h"
#include "se_core/error.h"
#include "se_platform/file_stream.h"
#include "se_platform/fs.h"

#ifndef SE_BENCH_CONFIG
#define SE_BENCH_CONFIG "unknown"
#endif

namespace {

struct Args {
  std::uint32_t iterations = 500;
  std::uint32_t warmup = 20;
  std::optional<double> max_p99_ms;
  std::string json;
  std::string scratch = ".";
};

struct Summary {
  std::string kind;
  std::size_t count = 0;
  double median_ms = 0.0;
  double p95_ms = 0.0;
  double p99_ms = 0.0;
  double max_ms = 0.0;
};

std::uint64_t NowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// Nearest rank.
double Percentile(std::vector<std::uint64_t> sorted, double q) {
  if (sorted.empty()) return 0.0;
  std::ranges::sort(sorted);
  const auto rank = static_cast<std::size_t>(
      q * static_cast<double>(sorted.size()) + 0.999999);
  const std::size_t index = std::clamp<std::size_t>(rank, 1, sorted.size()) - 1;
  return static_cast<double>(sorted[index]) * 1e-6;
}

Summary Summarize(std::string kind, const std::vector<std::uint64_t>& ns) {
  Summary summary;
  summary.kind = std::move(kind);
  summary.count = ns.size();
  summary.median_ms = Percentile(ns, 0.50);
  summary.p95_ms = Percentile(ns, 0.95);
  summary.p99_ms = Percentile(ns, 0.99);
  summary.max_ms = Percentile(ns, 1.0);
  return summary;
}

void Fail(const std::string& what) {
  std::fputs(("se_lane_bench: " + what + "\n").c_str(), stderr);
  std::exit(1);
}

template <class T>
T Must(std::expected<T, se::Error> value, std::string_view what) {
  if (!value) {
    Fail(std::format("{}: {} ({})", what, value.error().message,
                     value.error().path));
  }
  return std::move(*value);
}

void Must(std::expected<void, se::Error> value, std::string_view what) {
  if (!value) Fail(std::format("{}: {}", what, value.error().message));
}

std::vector<std::byte> Noise(std::size_t size, std::uint32_t seed) {
  std::vector<std::byte> bytes(size);
  std::uint32_t x = seed * 2654435761U + 1;
  for (std::byte& b : bytes) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    b = static_cast<std::byte>(x & 0xFF);
  }
  return bytes;
}

std::vector<std::uint64_t> BenchRead(const Args& args) {
  const std::string path = se::fs::Join(args.scratch, "lane_read.bin");
  // 64 MB for a gate run; a smoke run reads less.
  const std::size_t megabytes = std::clamp<std::size_t>(
      std::size_t{args.warmup} + args.iterations, 4, 64);
  Must(se::fs::WriteFile(path, Noise(megabytes << 20, 1)),
       "write the read file");
  std::vector<std::byte> buffer(se::kReadStepBytes);
  std::vector<std::uint64_t> samples;
  std::optional<se::fs::FileReader> reader;
  for (std::uint32_t i = 0; i < args.warmup + args.iterations; ++i) {
    if (!reader || reader->AtEnd()) {
      reader.emplace(Must(se::fs::FileReader::Open(path), "open"));
    }
    const std::uint64_t start = NowNs();
    const std::size_t got = Must(reader->Read(buffer), "read");
    const std::uint64_t took = NowNs() - start;
    if (got != buffer.size()) Fail("short read");
    if (i >= args.warmup) samples.push_back(took);
  }
  reader.reset();
  Must(se::fs::Remove(path), "remove the read file");
  return samples;
}

std::vector<std::uint64_t> BenchLz4(const Args& args) {
  // 64 blocks of 256 KB: 2048 x 2048 RGBA8 of sprite-like content (runs
  // of equal texels, some noise).
  se::AtlasPage page;
  page.width = 2048;
  page.height = 2048;
  const std::vector<std::byte> noise = Noise(page.Bytes() / 16, 2);
  page.pixels.resize(page.Bytes());
  for (std::size_t i = 0; i < page.pixels.size(); ++i) {
    page.pixels[i] = static_cast<std::uint8_t>(noise[i / 16]);
  }
  const std::vector<std::byte> encoded =
      Must(se::EncodeAtlasPage(page), "encode the page");
  std::vector<std::uint64_t> samples;
  std::optional<se::AtlasPageDecoder> decoder;
  for (std::uint32_t i = 0; i < args.warmup + args.iterations; ++i) {
    if (!decoder || decoder->Done()) {
      decoder.emplace(
          Must(se::AtlasPageDecoder::Begin(encoded, "bench"), "begin"));
    }
    const std::uint64_t start = NowNs();
    Must(decoder->DecodeNextBlock(), "decode");
    const std::uint64_t took = NowNs() - start;
    if (i >= args.warmup) samples.push_back(took);
  }
  return samples;
}

std::vector<std::uint64_t> BenchChunk(const Args& args) {
  // An entity as rampage's scenes write one: a guid, a name, a prefab and
  // a path override of five points.
  std::vector<std::string> entities;
  entities.reserve(256);
  for (int i = 0; i < 256; ++i) {
    entities.push_back(std::format(
        R"({{"guid":"5a4db0c5-0000-4000-8000-{:012x}","name":"enemy {}",)"
        R"("prefab":"prefabs/enemy.prefab.json","overrides":{{"Path":{{)"
        R"("points":[{{"x":{},"y":{}}},{{"x":{},"y":{}}},{{"x":{},"y":{}}},)"
        R"({{"x":{},"y":{}}},{{"x":{},"y":{}}}]}},"Stats":{{"HP":100,)"
        R"("Speed":500.5}}}}}})",
        i, i, (i * 37) % 1920, (i * 53) % 1080, (i * 71) % 1920,
        (i * 13) % 1080, (i * 29) % 1920, (i * 41) % 1080, (i * 7) % 1920,
        (i * 97) % 1080, (i * 11) % 1920, (i * 3) % 1080));
  }
  const std::string text = Must(se::WriteSceneChunk(0, entities), "chunk");
  std::vector<std::uint64_t> samples;
  for (std::uint32_t i = 0; i < args.warmup + args.iterations; ++i) {
    const std::uint64_t start = NowNs();
    const se::SceneChunkPayload chunk =
        Must(se::ParseSceneChunk(text, "bench"), "parse");
    const std::uint64_t took = NowNs() - start;
    if (chunk.entities.size() != entities.size()) Fail("parse lost entities");
    if (i >= args.warmup) samples.push_back(took);
  }
  return samples;
}

Args Parse(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (i + 1 >= argc) Fail(std::format("{} needs a value", flag));
    const std::string value = argv[++i];
    if (flag == "--iterations") {
      args.iterations = static_cast<std::uint32_t>(std::stoul(value));
    } else if (flag == "--warmup") {
      args.warmup = static_cast<std::uint32_t>(std::stoul(value));
    } else if (flag == "--max-p99-ms") {
      args.max_p99_ms = std::stod(value);
    } else if (flag == "--json") {
      args.json = value;
    } else if (flag == "--scratch") {
      args.scratch = value;
    } else {
      Fail(std::format("unknown option '{}'", flag));
    }
  }
  return args;
}

int Run(int argc, char** argv) {
  const Args args = Parse(argc, argv);
  const std::vector<Summary> results = {
      Summarize("read_1mb", BenchRead(args)),
      Summarize("lz4_block", BenchLz4(args)),
      Summarize("scene_chunk", BenchChunk(args))};
  bool passed = true;
  std::fputs(std::format("lane_steps ({} build): {} timed steps of each kind "
                         "after {} untimed\n",
                         SE_BENCH_CONFIG, args.iterations, args.warmup)
                 .c_str(),
             stdout);
  std::string json = std::format(
      "{{\n  \"suite\": \"lane_steps\",\n  \"config\": \"{}\",\n  "
      "\"iterations\": {},\n  \"warmup\": {},\n  \"max_p99_ms\": {},\n  "
      "\"steps\": [",
      SE_BENCH_CONFIG, args.iterations, args.warmup,
      args.max_p99_ms ? std::format("{:.6f}", *args.max_p99_ms) : "null");
  for (std::size_t i = 0; i < results.size(); ++i) {
    const Summary& s = results[i];
    const bool ok = !args.max_p99_ms || s.p99_ms <= *args.max_p99_ms;
    passed = passed && ok;
    std::fputs(
        std::format("  {:<12} median {:.4f} ms  p95 {:.4f} ms  p99 "
                    "{:.4f} ms  max {:.4f} ms{}\n",
                    s.kind, s.median_ms, s.p95_ms, s.p99_ms, s.max_ms,
                    args.max_p99_ms
                        ? std::format("  gate p99 <= {:.2f} ms: {}",
                                      *args.max_p99_ms, ok ? "ok" : "FAILED")
                        : std::string())
            .c_str(),
        stdout);
    json += std::format(
        "{}\n    {{\"kind\": \"{}\", \"count\": {}, \"median_ms\": {:.6f}, "
        "\"p95_ms\": {:.6f}, \"p99_ms\": {:.6f}, \"max_ms\": {:.6f}, "
        "\"passed\": {}}}",
        i == 0 ? "" : ",", s.kind, s.count, s.median_ms, s.p95_ms, s.p99_ms,
        s.max_ms, ok);
  }
  json += std::format("\n  ],\n  \"passed\": {}\n}}\n", passed);
  if (!args.json.empty()) {
    std::ofstream out(args.json, std::ios::binary);
    out << json;
  }
  return passed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  // A tool may catch (ADR-006 exempts test and tool code).
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::fputs((std::string("se_lane_bench: ") + error.what() + "\n").c_str(),
               stderr);
    return 1;
  }
}
