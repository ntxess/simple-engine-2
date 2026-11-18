// Copyright 2026 The simple-engine-2 Authors.
//
// Golden files for determinism tests. A golden is a list of 64-bit values
// (per-tick hashes, bit patterns) stored as text in
// tests/golden/<SE_GOLDEN_PLATFORM>/<name>.txt, one "index value" pair per
// line in hex. Goldens are kept per platform and compiler (windows-msvc,
// windows-clangcl, ...), because floating-point results may legitimately
// differ between them.
//
// Running a test with the environment variable SE_UPDATE_GOLDENS=1 rewrites
// the file instead of comparing; commit the result.

#ifndef TESTS_SUPPORT_GOLDEN_H_
#define TESTS_SUPPORT_GOLDEN_H_

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace se::testing {

struct GoldenResult {
  bool ok = false;
  // Why it failed, or a note when the golden was rewritten.
  std::string message;
};

// The directory of this build's goldens.
[[nodiscard]] std::string GoldenDir();

// The platform-compiler name, for example "windows-msvc".
[[nodiscard]] std::string_view GoldenPlatform();

// Compares |values| with the golden |name|, or rewrites it when
// SE_UPDATE_GOLDENS=1. Reports the first differing index.
[[nodiscard]] GoldenResult CheckGolden(std::string_view name,
                                       std::span<const std::uint64_t> values);

// True when SE_UPDATE_GOLDENS=1 is set.
[[nodiscard]] bool UpdatingGoldens();

// Reads an environment variable; empty when unset.
[[nodiscard]] std::string GetEnv(const char* name);

}  // namespace se::testing

#endif  // TESTS_SUPPORT_GOLDEN_H_
