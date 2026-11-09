// Copyright 2026 The simple-engine-2 Authors.

#include "se_log/log_worker.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <latch>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "se_core/breadcrumb.h"
#include "se_core/log.h"
#include "se_core/log_ring.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_os/thread.h"
#include "support/test_roles.h"

namespace se {
namespace {

std::string OutPath(std::string_view name) {
  std::string path = std::string(SE_TEST_OUT_DIR) + "/" + std::string(name);
  std::remove(path.c_str());
  for (int i = 1; i <= 3; ++i) {
    std::remove(path.substr(0, path.size() - 4)
                    .append(".")
                    .append(std::to_string(i))
                    .append(".log")
                    .c_str());
  }
  return path;
}

std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

std::vector<std::string> Containing(const std::vector<std::string>& lines,
                                    std::string_view text) {
  std::vector<std::string> found;
  for (const std::string& line : lines) {
    if (line.find(text) != std::string::npos) found.push_back(line);
  }
  return found;
}

// The time column in microseconds.
std::uint64_t TimeUs(const std::string& line) {
  const std::size_t dot = line.find('.');
  const std::size_t space = line.find(' ');
  return std::stoull(line.substr(0, dot)) * 1'000'000 +
         std::stoull(line.substr(dot + 1, space - dot - 1));
}

LogConfig FileOnly(const std::string& path) {
  return LogConfig{
      .file_path = path, .stderr_enabled = false, .rate_limit_per_second = 0};
}

// The main thread as the App sets it up: registered, with slot 0.
#define SE_TEST_MAIN_THREAD()                       \
  const testing::MainThreadScope main_registration; \
  const RoleScope main_role({.role = ThreadRole::kMain, .name = "main"})

// Waits until the worker has drained once and parked, so that with
// wake_on_push off nothing is drained again before Stop.
void WaitForFirstPark(const LogWorker& worker) {
  while (worker.Stats().parks == 0) std::this_thread::yield();
}

TEST_CASE("Records from 4 threads come out in timestamp order",
          "[log_worker][threads]") {
  SE_TEST_MAIN_THREAD();
  const std::string path = OutPath("order.log");
  auto worker = LogWorker::Start(FileOnly(path));
  REQUIRE(worker.has_value());
  constexpr int kThreads = 4;
  constexpr int kRecords = 400;
  std::latch go(kThreads);
  std::vector<Thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    auto thread = StartThread(
        {.role = ThreadRole::kBackground, .name = "writer"}, [&go, t] {
          go.arrive_and_wait();
          for (int i = 0; i < kRecords; ++i) SE_LOG_INFO("rec {} {}", t, i);
        });
    REQUIRE(thread.has_value());
    threads.push_back(std::move(*thread));
  }
  for (Thread& thread : threads) thread.Join();
  worker->Stop();

  const std::vector<std::string> lines = Containing(ReadLines(path), " rec ");
  REQUIRE(lines.size() == std::size_t{kThreads} * kRecords);
  std::vector<int> next(kThreads, 0);
  std::uint64_t previous = 0;
  for (const std::string& line : lines) {
    const std::uint64_t time = TimeUs(line);
    REQUIRE(time >= previous);
    previous = time;
    const std::size_t at = line.find(" rec ");
    const int t = std::stoi(line.substr(at + 5));
    const int i = std::stoi(line.substr(line.find(' ', at + 5) + 1));
    REQUIRE(t >= 0);
    REQUIRE(t < kThreads);
    // Each thread's records appear once each and in order.
    REQUIRE(i == next[static_cast<std::size_t>(t)]);
    ++next[static_cast<std::size_t>(t)];
    REQUIRE(line.find(" I bg ") != std::string::npos);
  }
  REQUIRE(worker->Stats().records >= std::uint64_t{kThreads} * kRecords);
}

TEST_CASE("A full ring drops and the worker reports the count",
          "[log_worker]") {
  SE_TEST_MAIN_THREAD();
  const std::string path = OutPath("drops.log");
  LogConfig config = FileOnly(path);
  // The worker stays parked until Stop, so the ring fills up.
  config.wake_on_push = false;
  config.wake_period_ms = 60'000;
  auto worker = LogWorker::Start(config);
  REQUIRE(worker.has_value());
  WaitForFirstPark(*worker);
  std::optional<std::uint32_t> slot;
  std::uint64_t dropped_before = 0;
  auto thread =
      StartThread({.role = ThreadRole::kBackground, .name = "dropper"}, [&] {
        slot = CurrentThreadSlot();
        dropped_before = CurrentThreadLogRing()->Dropped();
        for (int i = 0; i < 1500; ++i) SE_LOG_INFO("flood {}", i);
      });
  REQUIRE(thread.has_value());
  thread->Join();
  REQUIRE(slot.has_value());
  const LogRing* ring = ThreadRegistry::Instance().SlotLogRing(*slot);
  REQUIRE(ring != nullptr);
  REQUIRE(ring->Dropped() - dropped_before == 1500 - kThreadLogRingCapacity);
  worker->Stop();

  const std::vector<std::string> lines = ReadLines(path);
  REQUIRE(Containing(lines, " flood ").size() == kThreadLogRingCapacity);
  // The oldest records survive; the newest were dropped.
  REQUIRE(Containing(lines, " flood 1023").size() == 1);
  REQUIRE(Containing(lines, " flood 1024").empty());
  const std::vector<std::string> notices =
      Containing(lines, "476 records dropped on dropper");
  REQUIRE(notices.size() == 1);
  REQUIRE(notices[0].find(" W log ") != std::string::npos);
  REQUIRE(worker->Stats().dropped == 476);
}

TEST_CASE("A call site above 50 records per second is summarised",
          "[log_worker]") {
  SE_TEST_MAIN_THREAD();
  const std::string path = OutPath("rate.log");
  LogConfig config = FileOnly(path);
  config.rate_limit_per_second = 50;
  auto worker = LogWorker::Start(config);
  REQUIRE(worker.has_value());
  for (int i = 0; i < 200; ++i) SE_LOG_INFO("hot {}", i);
  SE_LOG_INFO("cold");
  worker->Stop();

  const std::vector<std::string> lines = ReadLines(path);
  // The burst takes far less than a second: 50 lines, then one summary.
  REQUIRE(Containing(lines, " hot ").size() == 50);
  const std::vector<std::string> summary =
      Containing(lines, "150 more records from log_worker_test.cc:");
  REQUIRE(summary.size() == 1);
  REQUIRE(summary[0].find("(rate limited)") != std::string::npos);
  REQUIRE(Containing(lines, " cold").size() == 1);
  REQUIRE(worker->Stats().suppressed == 150);
}

TEST_CASE("Stop drains every ring", "[log_worker]") {
  SE_TEST_MAIN_THREAD();
  const std::string path = OutPath("drain.log");
  LogConfig config = FileOnly(path);
  config.wake_on_push = false;
  config.wake_period_ms = 60'000;
  auto worker = LogWorker::Start(config);
  REQUIRE(worker.has_value());
  WaitForFirstPark(*worker);
  for (int i = 0; i < 100; ++i) SE_LOG_WARN("main {}", i);
  // A thread the engine did not start goes through the library ring.
  bool logged = false;
  std::thread library([&logged] {
    // Test code may catch (ADR-006): a throw must not end the thread.
    try {
      SE_LOG_WARN("from a library thread");
      logged = true;
    } catch (...) {
      logged = false;
    }
  });
  library.join();
  REQUIRE(logged);
  REQUIRE(ReadLines(path).empty());
  worker->Stop();
  REQUIRE_FALSE(worker->Running());
  const std::vector<std::string> lines = ReadLines(path);
  REQUIRE(Containing(lines, " main ").size() == 100);
  const std::vector<std::string> library_lines =
      Containing(lines, "from a library thread");
  REQUIRE(library_lines.size() == 1);
  REQUIRE(library_lines[0].find(" W lib ") != std::string::npos);
}

std::vector<std::string>& Captured() {
  static std::vector<std::string> lines;
  return lines;
}

void Capture(LogLevel /*level*/, std::string_view line) {
  Captured().emplace_back(line);
}

TEST_CASE("Without a worker the front end writes synchronously",
          "[log_worker]") {
  SE_TEST_MAIN_THREAD();
  Captured().clear();
  SetLogSink(&Capture);
  const std::string path = OutPath("fallback.log");
  SE_LOG_INFO("before start");
  REQUIRE(Captured().size() == 1);
  {
    auto worker = LogWorker::Start(FileOnly(path));
    REQUIRE(worker.has_value());
    REQUIRE(LogConsumerAttached());
    SE_LOG_INFO("while running");
    worker->Stop();
    REQUIRE_FALSE(LogConsumerAttached());
    SE_LOG_INFO("after stop");
  }
  SetLogSink(nullptr);
  REQUIRE(Captured().size() == 2);
  CHECK(Captured()[0].find("before start") != std::string::npos);
  CHECK(Captured()[1].find("after stop") != std::string::npos);
  const std::vector<std::string> lines = ReadLines(path);
  REQUIRE(Containing(lines, "while running").size() == 1);
  REQUIRE(Containing(lines, "before start").empty());
  REQUIRE(Containing(lines, "after stop").empty());
}

TEST_CASE("Lines carry time, level, role, tick, location and message",
          "[log_worker]") {
  SE_TEST_MAIN_THREAD();
  const std::string path = OutPath("format.log");
  auto worker = LogWorker::Start(FileOnly(path));
  REQUIRE(worker.has_value());
  ThreadBreadcrumb().SetTick(1440);
  SE_LOG_INFO("loaded {} entities", 17);
  ThreadBreadcrumb().SetTick(0);
  worker->Stop();
  const std::vector<std::string> lines = Containing(ReadLines(path), "loaded");
  REQUIRE(lines.size() == 1);
  const std::regex format(
      R"(^\d+\.\d{6} I main t=1440 log_worker_test\.cc:\d+ loaded 17 entities$)");
  CHECK(std::regex_match(lines[0], format));
}

TEST_CASE("The panel ring keeps the last lines", "[log_worker]") {
  SE_TEST_MAIN_THREAD();
  LogConfig config{
      .stderr_enabled = false, .panel_lines = 10, .rate_limit_per_second = 0};
  auto worker = LogWorker::Start(config);
  REQUIRE(worker.has_value());
  for (int i = 0; i < 25; ++i) SE_LOG_INFO("panel {}", i);
  worker->Stop();
  std::vector<std::string> lines;
  REQUIRE(worker->TryCopyPanelLines(lines));
  REQUIRE(lines.size() == 10);
  CHECK(lines.front().ends_with("panel 15"));
  CHECK(lines.back().ends_with("panel 24"));
}

TEST_CASE("Start fails cleanly", "[log_worker]") {
  SE_TEST_MAIN_THREAD();
  const std::string first_path = OutPath("first.log");
  auto first = LogWorker::Start(FileOnly(first_path));
  REQUIRE(first.has_value());
  const std::string second_path = OutPath("second.log");
  const auto second = LogWorker::Start(FileOnly(second_path));
  REQUIRE_FALSE(second.has_value());
  CHECK(second.error().code == ErrorCode::kUnsupported);
  // The second worker's thread ended without draining: the first one still
  // owns the rings and writes what comes next (ADR-045).
  CHECK(LogConsumerAttached());
  SE_LOG_INFO("after the refused start");
  first->Stop();
  REQUIRE(Containing(ReadLines(first_path), "after the refused start").size() ==
          1);
  CHECK(ReadLines(second_path).empty());

  // A log path below a regular file cannot be opened.
  const std::string blocker = OutPath("blocker.txt");
  {
    std::ofstream(blocker) << "not a directory\n";
  }
  const auto bad = LogWorker::Start(FileOnly(blocker + "/x.log"));
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code == ErrorCode::kIo);
  CHECK(bad.error().path == blocker + "/x.log");
  CHECK_FALSE(LogConsumerAttached());
}

// The worker's console lines reach the test's sink (WriteConsoleLine); this
// one holds the worker for 1.2 s on the line that asks for it, longer than
// the 1 s DetachLogConsumer used to wait (ADR-045).
std::atomic<bool> g_worker_stalled{false};

void StallingSink(LogLevel /*level*/, std::string_view line) {
  if (line.find("stall the worker") != std::string_view::npos) {
    g_worker_stalled.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  }
}

TEST_CASE("Stop waits for every write in progress without a deadline",
          "[log_worker][threads]") {
  SE_TEST_MAIN_THREAD();
  g_worker_stalled.store(false);
  SetLogSink(&StallingSink);
  LogConfig config{.stderr_enabled = true, .rate_limit_per_second = 0};
  auto worker = LogWorker::Start(config);
  REQUIRE(worker.has_value());
  SE_LOG_INFO("stall the worker");
  while (!g_worker_stalled.load()) std::this_thread::yield();
  // A thread with a ring of its own asks for a flush, which the stalled
  // worker serves only after the stall; FlushLog holds the thread's write
  // mark (and uses the consumer) until then.
  std::atomic<const LogRing*> ring{nullptr};
  std::atomic<bool> flushed{false};
  auto flusher = StartThread(
      {.role = ThreadRole::kBackground, .name = "flusher"}, [&ring, &flushed] {
        ring.store(CurrentThreadLogRing());
        flushed.store(FlushLog(10'000'000'000));
      });
  REQUIRE(flusher.has_value());
  while (ring.load() == nullptr || ring.load()->WriteStartedNs() == 0) {
    std::this_thread::yield();
  }
  worker->Stop();
  // Stop returned only after the flusher let go of the consumer, so the
  // worker's State could be freed now.
  REQUIRE(ring.load()->WriteStartedNs() == 0);
  flusher->Join();
  CHECK(flushed.load());
  SetLogSink(nullptr);
}

}  // namespace
}  // namespace se
