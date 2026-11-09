// Copyright 2026 The simple-engine-2 Authors.
//
// The unbiased clock, debugger detection and the executable's directory.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>

#include "se_os/debugger.h"
#include "se_os/process.h"
#include "se_os/unbiased_clock.h"

namespace se {
namespace {

TEST_CASE("The unbiased clock is monotonic and advances", "[os]") {
  const std::uint64_t first = UnbiasedNowNs();
  std::uint64_t previous = first;
  for (int i = 0; i < 1000; ++i) {
    const std::uint64_t now = UnbiasedNowNs();
    REQUIRE(now >= previous);
    previous = now;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const std::uint64_t later = UnbiasedNowNs();
  REQUIRE(later - first >= 10'000'000);
  REQUIRE(later - first < 10'000'000'000);
}

TEST_CASE("No debugger is attached under CTest", "[os]") {
  CHECK_FALSE(DebuggerAttached());
}

TEST_CASE("ExecutableDir holds the test executable", "[os]") {
  const auto dir = ExecutableDir();
  REQUIRE(dir.has_value());
  REQUIRE_FALSE(dir->empty());
  REQUIRE(dir->back() != '/');
  REQUIRE(dir->back() != '\\');
#if defined(_WIN32)
  const std::string exe = *dir + "/se_os_tests.exe";
#else
  const std::string exe = *dir + "/se_os_tests";
#endif
  const std::ifstream file(exe, std::ios::binary);
  CHECK(file.good());
}

}  // namespace
}  // namespace se
