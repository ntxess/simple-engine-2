// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/log.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/breadcrumb.h"
#include "se_core/thread_role.h"

namespace se {
namespace {

std::vector<std::string>& Captured() {
  static std::vector<std::string> lines;
  return lines;
}

void CaptureSink(LogLevel /*level*/, std::string_view line) {
  Captured().emplace_back(line);
}

// Installs the capturing sink for one test.
class CaptureScope {
 public:
  CaptureScope() : previous_level_(GetLogLevel()) {
    Captured().clear();
    SetLogSink(&CaptureSink);
  }
  ~CaptureScope() {
    SetLogSink(nullptr);
    SetLogLevel(previous_level_);
  }
  CaptureScope(const CaptureScope&) = delete;
  CaptureScope& operator=(const CaptureScope&) = delete;

 private:
  LogLevel previous_level_;
};

TEST_CASE("Log lines carry level, tick, role and location", "[log]") {
  CaptureScope capture;
  SetCurrentRole(ThreadRole::kSim);
  ThreadBreadcrumb().SetTick(1234);
  SE_LOG_INFO("spawned {} movers at {:.1f}", 3, 2.5);
  REQUIRE(Captured().size() == 1);
  const std::string& line = Captured()[0];
  CHECK(line.starts_with("[I t=1234 sim] spawned 3 movers at 2.5 ("));
  CHECK(line.find("log_test.cc:") != std::string::npos);
  SetCurrentRole(ThreadRole::kLibrary);
  ThreadBreadcrumb().SetTick(0);
}

TEST_CASE("The run-time level filters messages", "[log]") {
  CaptureScope capture;
  SetLogLevel(LogLevel::kWarn);
  SE_LOG_INFO("hidden");
  SE_LOG_WARN("shown");
  SE_LOG_ERROR("shown {}", "too");
  REQUIRE(Captured().size() == 2);
  CHECK_FALSE(LogEnabled(LogLevel::kDebug));
  CHECK(LogEnabled(LogLevel::kError));
}

TEST_CASE("Levels below SE_LOG_MIN_LEVEL are stripped", "[log]") {
  CaptureScope capture;
  SetLogLevel(LogLevel::kTrace);
  SE_LOG_TRACE("trace {}", 1);
  SE_LOG_DEBUG("debug {}", 2);
#if SE_ASSERTS
  REQUIRE(kLogMinLevel == 0);
  REQUIRE(Captured().size() == 2);
#else
  REQUIRE(kLogMinLevel == 2);
  CHECK(Captured().empty());
#endif
}

TEST_CASE("Long messages are truncated, not dropped", "[log]") {
  CaptureScope capture;
  const std::string long_text(2 * kLogMaxMessage, 'x');
  SE_LOG_WARN("{}", long_text);
  REQUIRE(Captured().size() == 1);
  CHECK(Captured()[0].find(std::string(kLogMaxMessage, 'x')) !=
        std::string::npos);
  CHECK(Captured()[0].find(std::string(kLogMaxMessage + 1, 'x')) ==
        std::string::npos);
}

}  // namespace
}  // namespace se
