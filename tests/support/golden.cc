// Copyright 2026 The simple-engine-2 Authors.

#include "support/golden.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#ifndef SE_GOLDEN_ROOT
#error "SE_GOLDEN_ROOT must name tests/golden"
#endif
#ifndef SE_GOLDEN_PLATFORM
#error "SE_GOLDEN_PLATFORM must name the platform-compiler pair"
#endif

namespace se::testing {
namespace {

void MakeDir(const std::string& path) {
#if defined(_WIN32)
  static_cast<void>(_mkdir(path.c_str()));
#else
  static_cast<void>(mkdir(path.c_str(), 0755));
#endif
}

std::vector<std::uint64_t> ReadValues(std::ifstream& in) {
  std::vector<std::uint64_t> values;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string index;
    std::string value;
    fields >> index >> value;
    values.push_back(std::stoull(value, nullptr, 16));
  }
  return values;
}

}  // namespace

std::string GetEnv(const char* name) {
#if defined(_MSC_VER)
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return {};
  std::string result(value);
  std::free(value);
  return result;
#else
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
#endif
}

bool UpdatingGoldens() { return GetEnv("SE_UPDATE_GOLDENS") == "1"; }

std::string_view GoldenPlatform() { return SE_GOLDEN_PLATFORM; }

std::string GoldenDir() {
  return std::string(SE_GOLDEN_ROOT) + "/" + SE_GOLDEN_PLATFORM;
}

GoldenResult CheckGolden(std::string_view name,
                         std::span<const std::uint64_t> values) {
  const std::string path = GoldenDir() + "/" + std::string(name) + ".txt";
  if (UpdatingGoldens()) {
    MakeDir(SE_GOLDEN_ROOT);
    MakeDir(GoldenDir());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "# golden " << name << " (" << SE_GOLDEN_PLATFORM
        << "); rewrite with SE_UPDATE_GOLDENS=1\n";
    for (std::size_t i = 0; i < values.size(); ++i) {
      out << std::format("{} {:016x}\n", i, values[i]);
    }
    if (!out) return {false, "could not write " + path};
    return {true, "rewrote " + path};
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {false, "missing golden " + path +
                       " (run the test with SE_UPDATE_GOLDENS=1)"};
  }
  const std::vector<std::uint64_t> expected = ReadValues(in);
  const std::size_t common = std::min(expected.size(), values.size());
  for (std::size_t i = 0; i < common; ++i) {
    if (expected[i] != values[i]) {
      return {false, std::format("{}: first difference at index {}: golden "
                                 "{:016x}, got {:016x}",
                                 path, i, expected[i], values[i])};
    }
  }
  if (expected.size() != values.size()) {
    return {false, std::format("{}: golden has {} values, got {}", path,
                               expected.size(), values.size())};
  }
  return {true, {}};
}

}  // namespace se::testing
