// Copyright 2026 The simple-engine-2 Authors.
//
// The background lane (docs/milestone-specs.md M9): rings, the semaphore,
// classes and QoS, bounded steps, quit joining within one step, strands,
// revisions, Durable jobs on a throttled disk, the sharing-violation
// retry, the hot-reload scan and the fault boundary. Run under TSan in WSL
// (linux-clang-tsan).

#include <SDL3/SDL_iostream.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "assets/support/lane_test_support.h"
#include "se_assets/atlas_page.h"
#include "se_assets/background_lane.h"
#include "se_assets/lane_jobs.h"
#include "se_assets/payloads.h"
#include "se_core/thread_role.h"
#include "se_platform/file_stream.h"
#include "se_platform/fs.h"
#include "se_platform/fs_faults.h"
#include "se_runtime/lane_quit.h"
#include "se_runtime/quit.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::DrainCount;
using testing::DrainUntil;
using testing::MainThreadScope;
using testing::NowNs;
using testing::PatternBytes;
using testing::ReadBytes;
using testing::ScratchDir;
using testing::TextBytes;
using testing::WriteBytes;

#ifndef SE_TEST_SANITIZED
#define SE_TEST_SANITIZED 0
#endif
constexpr bool kSanitized = SE_TEST_SANITIZED != 0;

void NoWake() noexcept {}

LaneConfig TestConfig() {
  LaneConfig config;
  config.wake_main = &NoWake;
  config.scan_period_ns = 0;
  config.trace_capacity = 1 << 16;
  return config;
}

std::unique_ptr<BackgroundLane> StartLane(const LaneConfig& config) {
  auto lane = BackgroundLane::Start(config);
  REQUIRE(lane.has_value());
  return std::move(*lane);
}

// A job of |steps| steps that records the role it ran on.
class CountingJob final : public LaneJob {
 public:
  CountingJob(int steps, std::atomic<int>* lane_role_steps)
      : steps_(steps), lane_role_steps_(lane_role_steps) {}
  [[nodiscard]] std::string_view Kind() const noexcept override {
    return "count";
  }
  StepResult Step(LaneContext& /*lane*/) override {
    if (CurrentRole() == ThreadRole::kBackground && lane_role_steps_) {
      lane_role_steps_->fetch_add(1);
    }
    return ++done_ >= steps_ ? StepResult::Done() : StepResult::Continue();
  }
  std::expected<LanePayload, Error> TakeResult() override {
    return LanePayload(std::vector<std::byte>(static_cast<std::size_t>(done_)));
  }

 private:
  int steps_;
  int done_ = 0;
  std::atomic<int>* lane_role_steps_;
};

// Steps that each take about |step_ms| (a slow decode).
class SlowJob final : public LaneJob {
 public:
  SlowJob(int steps, int step_ms) : steps_(steps), step_ms_(step_ms) {}
  [[nodiscard]] std::string_view Kind() const noexcept override {
    return "slow";
  }
  StepResult Step(LaneContext& /*lane*/) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms_));
    return ++done_ >= steps_ ? StepResult::Done() : StepResult::Continue();
  }
  std::expected<LanePayload, Error> TakeResult() override {
    return LanePayload(std::monostate{});
  }

 private:
  int steps_;
  int step_ms_;
  int done_ = 0;
};

class ThrowingJob final : public LaneJob {
 public:
  [[nodiscard]] std::string_view Kind() const noexcept override {
    return "throws";
  }
  StepResult Step(LaneContext& /*lane*/) override {
    throw std::runtime_error("injected");
  }
  std::expected<LanePayload, Error> TakeResult() override {
    return LanePayload(std::monostate{});
  }
};

// A step that waits until |release| is set (the lane is busy in it).
class GateJob final : public LaneJob {
 public:
  GateJob(std::atomic<bool>* entered, std::atomic<bool>* release)
      : entered_(entered), release_(release) {}
  [[nodiscard]] std::string_view Kind() const noexcept override {
    return "gate";
  }
  StepResult Step(LaneContext& /*lane*/) override {
    entered_->store(true);
    entered_->notify_all();
    release_->wait(false);
    return StepResult::Done();
  }
  std::expected<LanePayload, Error> TakeResult() override {
    return LanePayload(std::monostate{});
  }

 private:
  std::atomic<bool>* entered_;
  std::atomic<bool>* release_;
};

JobDesc Desc(JobClass job_class, bool durable = false) {
  JobDesc desc;
  desc.job_class = job_class;
  desc.durable = durable;
  return desc;
}

TEST_CASE("Jobs run in steps on the lane and complete on main", "[lane]") {
  const MainThreadScope main;
  auto lane = StartLane(TestConfig());
  std::atomic<int> lane_steps{0};
  const Ticket a = lane->Submit(Desc(JobClass::kBackground),
                                std::make_unique<CountingJob>(5, &lane_steps));
  const Ticket b = lane->Submit(Desc(JobClass::kUrgent),
                                std::make_unique<CountingJob>(3, &lane_steps));
  std::vector<LaneCompletion> done;
  REQUIRE(DrainCount(*lane, done, 2));
  REQUIRE(lane_steps.load() == 8);
  std::map<Ticket, std::uint32_t> steps;
  for (const LaneCompletion& completion : done) {
    REQUIRE(completion.result.has_value());
    steps[completion.ticket] = completion.steps;
  }
  REQUIRE(steps[a] == 5);
  REQUIRE(steps[b] == 3);
  lane->Stop();
  CHECK_FALSE(lane->Running());
  const LaneStats stats = lane->Stats();
  REQUIRE(stats.completed == 2);
  REQUIRE(stats.steps == 8);
  REQUIRE(stats.urgent_steps == 3);
  REQUIRE(stats.background_steps == 5);
  // Urgent and Background steps ran at their own QoS.
  REQUIRE(stats.qos_switches >= 1);
}

TEST_CASE(
    "A full request ring keeps submits on main; a full completion ring "
    "keeps completions on the lane",
    "[lane]") {
  const MainThreadScope main;
  LaneConfig config = TestConfig();
  config.request_capacity = 8;
  config.completion_capacity = 2;
  auto lane = StartLane(config);
  constexpr int kJobs = 64;
  for (int i = 0; i < kJobs; ++i) {
    lane->Submit(Desc(JobClass::kBackground),
                 std::make_unique<CountingJob>(1, nullptr));
  }
  // The lane finishes the 8 it has while main does not drain: 6 wait on
  // the lane for room.
  const std::uint64_t start = NowNs();
  while (lane->Stats().completions_held < 6 &&
         NowNs() - start < 10'000'000'000ULL) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::vector<LaneCompletion> done;
  REQUIRE(DrainCount(*lane, done, kJobs));
  // Oldest first.
  for (std::size_t i = 1; i < done.size(); ++i) {
    CHECK(done[i - 1].ticket < done[i].ticket);
  }
  const LaneStats stats = lane->Stats();
  REQUIRE(stats.submits_queued > 0);
  REQUIRE(stats.completions_held > 0);
}

TEST_CASE("A job that throws completes with an Error and the lane runs on",
          "[lane]") {
  const MainThreadScope main;
  auto lane = StartLane(TestConfig());
  const Ticket bad =
      lane->Submit(Desc(JobClass::kUrgent), std::make_unique<ThrowingJob>());
  const Ticket good = lane->Submit(Desc(JobClass::kUrgent),
                                   std::make_unique<CountingJob>(2, nullptr));
  std::vector<LaneCompletion> done;
  REQUIRE(DrainCount(*lane, done, 2));
  for (const LaneCompletion& completion : done) {
    if (completion.ticket == bad) {
      REQUIRE_FALSE(completion.result.has_value());
      CHECK(completion.result.error().message.find("injected") !=
            std::string::npos);
    } else {
      CHECK(completion.ticket == good);
      CHECK(completion.result.has_value());
    }
  }
  REQUIRE(lane->Stats().failed == 1);
}

// Quit during a large job joins within one step (the M9 exit). Every
// attempt: no step of the job begins after the stop but the one the lane
// had already picked, and the lane exits right after the step in progress.
// The join's wall time is gated at one step plus 5 ms; another program
// loading the machine can delay the lane's thread past that, so an attempt
// over the bound is reported and repeated (three attempts). An attempt
// whose job had ended before the stop was requested is repeated without
// counting (at most ten). Sanitizer
// builds report it: their allocators take milliseconds to free the
// cancelled job's buffers (SE_TEST_SANITIZED).
struct QuitAttempt {
  std::uint64_t stop_ns = 0;
  LaneStats stats;
};

template <class MakeJob>
void CheckQuitWithinOneStep(MakeJob make_job, std::uint64_t min_steps)
    SE_REQUIRES(g_main) {
  bool within = false;
  int early = 0;
  int attempt = 1;
  while (attempt <= 3 && !within) {
    auto lane = StartLane(TestConfig());
    lane->Submit(Desc(JobClass::kBackground), make_job());
    const std::uint64_t wait_start = NowNs();
    while (lane->Stats().steps < min_steps &&
           NowNs() - wait_start < 30'000'000'000ULL) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    REQUIRE(lane->Stats().steps >= min_steps);
    const std::uint64_t stop_start = NowNs();
    lane->Stop();
    const std::uint64_t stop_ns = NowNs() - stop_start;
    const LaneStats stats = lane->Stats();
    const double ms = 1e-6;
    WARN("attempt " << attempt << ": Stop() took "
                    << static_cast<double>(stop_ns) * ms
                    << " ms; the longest step "
                    << static_cast<double>(stats.max_step_ns) * ms
                    << " ms; the step in progress ended "
                    << static_cast<double>(stats.stop_to_last_step_ns) * ms
                    << " ms and the lane exited "
                    << static_cast<double>(stats.stop_to_exit_ns) * ms
                    << " ms after the request; " << stats.steps << " steps, "
                    << stats.steps_after_stop << " begun after the stop");
    if (stats.cancelled == 0 && stats.completed == 1 && early < 10) {
      // The job ran to its end before the request: main's poll sleeps, and
      // a 64 MB read from the file cache can end within one sleep (about
      // one run in four when the test runs alone; it failed the gate once
      // beside parallel tests). Such an attempt says nothing about a quit
      // during the job, so it is repeated and does not count.
      ++early;
      WARN("the job ended before the stop (" << early << " so far)");
      continue;
    }
    REQUIRE(stats.cancelled >= 1);
    REQUIRE(stats.completed == 0);
    REQUIRE(stats.steps_after_stop <= 1);
    within = stop_ns <= stats.max_step_ns + 5'000'000 &&
             stats.stop_to_last_step_ns <= stats.max_step_ns + 5'000'000;
    ++attempt;
  }
  if (!within && kSanitized) {
    WARN("over one step plus 5 ms in a sanitizer build: reported, not gated");
    return;
  }
  CHECK(within);
}

TEST_CASE("Quit during a 64 MB read joins within one step", "[lane][quit]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("quit_read");
  const std::string path = fs::Join(dir, "big.bin");
  REQUIRE(WriteBytes(path, PatternBytes(std::size_t{64} << 20, 7)));
  CheckQuitWithinOneStep([&path] { return MakeReadJob(path); }, 8);
}

TEST_CASE("Quit during a 200-block LZ4 decode joins within one step",
          "[lane][quit]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("quit_decode");
  // 200 blocks of 256 KB: 3200 x 4096 RGBA8.
  AtlasPage page;
  page.width = 3200;
  page.height = 4096;
  const std::vector<std::byte> noise = PatternBytes(page.Bytes(), 3);
  page.pixels.resize(page.Bytes());
  for (std::size_t i = 0; i < page.pixels.size(); ++i) {
    // Compressible, not trivially: runs of 8 equal bytes.
    page.pixels[i] = static_cast<std::uint8_t>(noise[i / 8]);
  }
  auto encoded = EncodeAtlasPage(page, kAtlasBlockBytes, /*level=*/1);
  REQUIRE(encoded.has_value());
  const std::string path = fs::Join(dir, "big.seatlas");
  REQUIRE(WriteBytes(path, *encoded));
  // The file's read steps, then well into the blocks.
  const auto read_steps = encoded->size() / kReadStepBytes + 2;
  CheckQuitWithinOneStep(
      [&path] {
        AtlasRequest request;
        request.page_path = path;
        request.mode = AtlasRequest::Mode::kPage;
        return MakeAtlasJob(std::move(request));
      },
      read_steps + 60);
}

TEST_CASE("Each read step reads at most 1 MB", "[lane]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("read_steps");
  const std::string path = fs::Join(dir, "five.bin");
  const std::vector<std::byte> bytes =
      PatternBytes((std::size_t{5} << 20) + 17, 9);
  REQUIRE(WriteBytes(path, bytes));
  auto lane = StartLane(TestConfig());
  lane->Submit(Desc(JobClass::kBackground), MakeReadJob(path));
  std::vector<LaneCompletion> done;
  REQUIRE(DrainCount(*lane, done, 1));
  lane->Stop();
  REQUIRE(done[0].result.has_value());
  CHECK(std::get<std::vector<std::byte>>(*done[0].result) == bytes);
  // An open step, then six reads of at most 1 MB.
  REQUIRE(done[0].steps == 7);
}

TEST_CASE("Urgent steps always go first; strands keep per-path order",
          "[lane][strand]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("strands");
  const std::string a = fs::Join(dir, "a.txt");
  const std::string b = fs::Join(dir, "b.txt");
  auto lane = StartLane(TestConfig());
  std::vector<std::pair<Ticket, std::string>> background;
  std::vector<Ticket> urgent;
  constexpr int kSaves = 8;
  for (int i = 0; i < kSaves; ++i) {
    // Several steps each (2.5 MB: three writes, a flush, a rename).
    for (const std::string& path : {a, b}) {
      SaveRequest save;
      save.path = path;
      save.revision = static_cast<std::uint64_t>(i) + 1;
      save.bytes =
          PatternBytes((std::size_t{5} << 19) + static_cast<std::size_t>(i),
                       static_cast<std::uint32_t>(i));
      std::string text = std::to_string(i);
      for (std::size_t c = 0; c < text.size(); ++c) {
        save.bytes[c] = static_cast<std::byte>(text[c]);
      }
      save.bytes[text.size()] = std::byte{'\n'};
      background.emplace_back(SubmitSave(*lane, std::move(save)), path);
    }
    // Urgent work arrives while the saves run.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    urgent.push_back(
        lane->Submit(Desc(JobClass::kUrgent), std::make_unique<SlowJob>(3, 1)));
  }
  std::vector<LaneCompletion> done;
  REQUIRE(DrainCount(*lane, done, background.size() + urgent.size()));
  lane->Stop();

  // The --lane-step-trace text: a header, then one line per step.
  const std::string trace = fs::Join(dir, "trace.txt");
  REQUIRE(lane->WriteStepTrace(trace).has_value());
  auto trace_text = fs::ReadTextFile(trace);
  REQUIRE(trace_text.has_value());
  CHECK(trace_text->starts_with("# lane step trace"));
  const auto lines =
      static_cast<std::size_t>(std::ranges::count(*trace_text, '\n'));
  REQUIRE(lines == lane->StepTrace().size() + 1);
  // Urgent first: no Background step ran while an Urgent one was ready.
  std::uint64_t background_steps = 0;
  for (const LaneStepRecord& record : lane->StepTrace()) {
    if (record.step_class == JobClass::kBackground) {
      ++background_steps;
      REQUIRE(record.urgent_ready == 0);
    }
  }
  REQUIRE(background_steps > 0);
  // Per path, completions arrive in submission order.
  std::map<std::string, std::vector<Ticket>> order;
  for (const LaneCompletion& completion : done) {
    REQUIRE(completion.result.has_value());
    if (completion.kind != "save") continue;
    const auto& outcome = std::get<SaveOutcome>(*completion.result);
    CHECK_FALSE(outcome.superseded);
    order[outcome.path].push_back(completion.ticket);
  }
  for (const auto& [path, tickets] : order) {
    CHECK(tickets.size() == kSaves);
    CHECK(std::ranges::is_sorted(tickets));
  }
  // The last save of each path is the file.
  CHECK(
      testing::BytesText(ReadBytes(a)).starts_with(std::to_string(kSaves - 1)));
  CHECK(
      testing::BytesText(ReadBytes(b)).starts_with(std::to_string(kSaves - 1)));
}

TEST_CASE("A strand's oldest job runs Urgent when Urgent work waits behind it",
          "[lane][strand]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("strand_boost");
  const std::string path = fs::Join(dir, "doc.json");
  auto lane = StartLane(TestConfig());
  // A long Background job to keep the lane busy, then a Background save
  // and an Urgent save of one path.
  lane->Submit(Desc(JobClass::kBackground), std::make_unique<SlowJob>(20, 1));
  SaveRequest first;
  first.path = path;
  first.revision = 1;
  first.bytes = TextBytes("{\"rev\":1}");
  const Ticket background = SubmitSave(*lane, std::move(first));
  SaveRequest second;
  second.path = path;
  second.revision = 2;
  second.bytes = TextBytes("{\"rev\":2}");
  const Ticket urgent = SubmitSave(*lane, std::move(second), JobClass::kUrgent);
  std::vector<LaneCompletion> done;
  REQUIRE(DrainCount(*lane, done, 3));
  lane->Stop();
  // Both saves finished before the slow Background job, in strand order.
  std::vector<Ticket> order;
  order.reserve(done.size());
  for (const LaneCompletion& completion : done) {
    order.push_back(completion.ticket);
  }
  REQUIRE(order.size() == 3);
  CHECK(order[0] == background);
  CHECK(order[1] == urgent);
  REQUIRE(testing::BytesText(ReadBytes(path)) == "{\"rev\":2}");
  bool boosted = false;
  for (const LaneStepRecord& record : lane->StepTrace()) {
    if (record.ticket == background && record.step_class == JobClass::kUrgent) {
      boosted = true;
    }
  }
  CHECK(boosted);
}

TEST_CASE("Out-of-order completion never renames over a newer revision",
          "[lane][revision]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("revisions");
  const std::string path = fs::Join(dir, "doc.json");
  auto lane = StartLane(TestConfig());
  // Revision 2 completes first: it was submitted first.
  SaveRequest newer;
  newer.path = path;
  newer.revision = 2;
  newer.bytes = TextBytes("{\"rev\":2}");
  const Ticket second = SubmitSave(*lane, std::move(newer));
  SaveRequest older;
  older.path = path;
  older.revision = 1;
  older.bytes = TextBytes("{\"rev\":1}");
  const Ticket first = SubmitSave(*lane, std::move(older));
  std::vector<LaneCompletion> done;
  REQUIRE(DrainCount(*lane, done, 2));
  lane->Stop();
  REQUIRE(done[0].ticket == second);
  REQUIRE(done[1].ticket == first);
  CHECK_FALSE(std::get<SaveOutcome>(*done[0].result).superseded);
  const auto& stale = std::get<SaveOutcome>(*done[1].result);
  CHECK(stale.superseded);
  REQUIRE(stale.revision == 1);
  REQUIRE(testing::BytesText(ReadBytes(path)) == "{\"rev\":2}");
  // No temporary file is left behind.
  auto entries = fs::ListDirectory(dir);
  REQUIRE(entries.has_value());
  REQUIRE(entries->size() == 1);
}

TEST_CASE("Durable saves survive quit on a throttled disk", "[lane][durable]") {
  if constexpr (!fs::kFsFaultHooks) {
    SKIP("the se::fs fault injector is compiled out of Release builds");
  }
  const MainThreadScope main;
  const std::string dir = ScratchDir("durable");
  const std::string path = fs::Join(dir, "chunk.json");
  // A scene chunk of about 4 MB: four write steps of 1 MB at 60 ms each.
  std::vector<std::string> entities;
  entities.reserve(40000);
  for (int i = 0; i < 40000; ++i) {
    entities.push_back(
        std::format("{{\"guid\":{},\"name\":\"entity "
                    "{}\",\"x\":{},\"y\":{},\"pad\":\"{}\"}}",
                    1000 + i, i, i * 3, i * 7, std::string(40, 'p')));
  }
  auto text = WriteSceneChunk(0, entities);
  REQUIRE(text.has_value());
  REQUIRE(text->size() > (std::size_t{3} << 20));

  fs::SetFsFaults(fs::FsFaults{.write_step_delay_ms = 60});
  auto lane = StartLane(TestConfig());
  LaneQuitProvider provider(*lane);
  QuitController quit;
  quit.AddProvider(&provider);
  SaveRequest save;
  save.path = path;
  save.revision = 1;
  save.durable = true;
  save.bytes = TextBytes(*text);
  SubmitSave(*lane, std::move(save));
  REQUIRE(lane->DurablePending() == 1);

  quit.Request(QuitReason::kRequested);
  const bool file_at_quit = fs::Exists(path);
  std::uint32_t pending_polls = 0;
  bool ready_without_file = false;
  const std::uint64_t start = NowNs();
  std::uint64_t ready_after_ns = 0;
  while (NowNs() - start < 20'000'000'000ULL) {
    quit.Poll();
    if (quit.ReadyToExit()) {
      // The lane renames before it counts the job finished, so a ready
      // quit always finds the file in place.
      ready_without_file = !fs::Exists(path);
      break;
    }
    ++pending_polls;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ready_after_ns = NowNs() - start;
  fs::ClearFsFaults();
  REQUIRE(quit.ReadyToExit());
  // The quit stayed pending until the rename: at least the four throttled
  // write steps (4 x 60 ms); the file was not there when it began.
  CHECK_FALSE(file_at_quit);
  CHECK_FALSE(ready_without_file);
  REQUIRE(pending_polls >= 2);
  REQUIRE(ready_after_ns >= 240'000'000ULL);
  REQUIRE(lane->DurablePending() == 0);
  lane->Stop();
  // The file is complete and parses.
  auto read = fs::ReadTextFile(path);
  REQUIRE(read.has_value());
  CHECK(read->size() == text->size());
  auto parsed = ParseSceneChunk(*read, path);
  REQUIRE(parsed.has_value());
  CHECK(parsed->entities.size() == entities.size());
}

// A quit provider standing for the user's answer to a prompt.
class AnswerProvider final : public QuitProvider {
 public:
  QuitPoll PollQuit(QuitReason /*reason*/) override {
    return cancel ? QuitPoll::kCancel : QuitPoll::kReady;
  }
  [[nodiscard]] std::string_view Name() const override { return "the user"; }
  bool cancel = true;
};

TEST_CASE("A Durable failure between two quits does not cancel the second",
          "[lane][durable]") {
  const MainThreadScope main;
  auto lane = StartLane(TestConfig());
  LaneQuitProvider provider(*lane);
  AnswerProvider user;
  QuitController quit;
  quit.AddProvider(&provider);
  quit.AddProvider(&user);
  // A Durable job holds the lane, so the first quit waits for it ...
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  lane->Submit(Desc(JobClass::kBackground, /*durable=*/true),
               std::make_unique<GateJob>(&entered, &release));
  entered.wait(false);
  // ... and a Durable save behind it will fail (no such directory).
  SaveRequest save;
  save.path = fs::Join(ScratchDir("between_quits"), "missing/save.bin");
  save.revision = 1;
  save.durable = true;
  save.bytes = TextBytes("lost");
  SubmitSave(*lane, std::move(save));
  // The lane's provider is polled first (pending), then the user calls the
  // quit off.
  quit.Request(QuitReason::kRequested);
  quit.Poll();
  REQUIRE(quit.Cancellations() == 1);
  REQUIRE(quit.Blocker() == "the user");
  REQUIRE_FALSE(quit.Pending());
  // Between the quits the save fails.
  release.store(true);
  release.notify_all();
  std::vector<LaneCompletion> done;
  REQUIRE(DrainCount(*lane, done, 2));
  REQUIRE(lane->DurableFailures() == 1);
  REQUIRE(lane->DurablePending() == 0);
  // The second quit finds nothing pending and nothing failed during it.
  user.cancel = false;
  quit.Request(QuitReason::kRequested);
  quit.Poll();
  CHECK(quit.ReadyToExit());
  REQUIRE(quit.Cancellations() == 1);
  lane->Stop();
}

TEST_CASE("Stop finishes Durable jobs and cancels the others",
          "[lane][durable]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("durable_stop");
  const std::string path = fs::Join(dir, "settings.json");
  fs::SetFsFaults(fs::FsFaults{.write_step_delay_ms = 30});
  auto lane = StartLane(TestConfig());
  lane->Submit(Desc(JobClass::kBackground), std::make_unique<SlowJob>(500, 1));
  SaveRequest save;
  save.path = path;
  save.revision = 1;
  save.durable = true;
  save.bytes = PatternBytes(std::size_t{3} << 20, 11);
  const std::vector<std::byte> expected = save.bytes;
  SubmitSave(*lane, std::move(save));
  lane->Stop();
  fs::ClearFsFaults();
  REQUIRE(lane->DurablePending() == 0);
  REQUIRE(lane->DurableFailures() == 0);
  REQUIRE(lane->Stats().cancelled == 1);
  CHECK(ReadBytes(path) == expected);
}

TEST_CASE("Stop runs Durable submits still queued on main", "[lane][durable]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("durable_queued");
  LaneConfig config = TestConfig();
  // A two-entry request ring, so submits queue on main behind it.
  config.request_capacity = 2;
  auto lane = StartLane(config);
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  lane->Submit(Desc(JobClass::kBackground),
               std::make_unique<GateJob>(&entered, &release));
  entered.wait(false);
  // The lane is inside the gate's step: nothing pulls the ring.
  constexpr int kSaves = 8;
  for (int i = 0; i < kSaves; ++i) {
    SaveRequest save;
    save.path = fs::Join(dir, std::format("save{}.bin", i));
    save.revision = 1;
    save.durable = true;
    save.bytes = PatternBytes(4096, static_cast<std::uint32_t>(i));
    SubmitSave(*lane, std::move(save));
  }
  // Not Durable: cancelled by the stop, wherever it waits.
  lane->Submit(Desc(JobClass::kBackground), std::make_unique<SlowJob>(5, 1));
  REQUIRE(lane->Stats().submits_queued >= kSaves - 2);
  REQUIRE(lane->DurablePending() == kSaves);
  // The gate opens while Stop waits for room in the ring.
  std::jthread opener([&release] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    release.store(true);
    release.notify_all();
  });
  lane->Stop();
  opener.join();
  REQUIRE(lane->DurableFailures() == 0);
  REQUIRE(lane->DurablePending() == 0);
  REQUIRE(lane->Stats().cancelled >= 1);
  for (int i = 0; i < kSaves; ++i) {
    INFO("save " << i);
    CHECK(ReadBytes(fs::Join(dir, std::format("save{}.bin", i))) ==
          PatternBytes(4096, static_cast<std::uint32_t>(i)));
  }
}

TEST_CASE("A save cancelled at Stop leaves no temporary file",
          "[lane][durable]") {
  if constexpr (!fs::kFsFaultHooks) {
    SKIP("the se::fs fault injector is compiled out of Release builds");
  }
  const MainThreadScope main;
  const std::string dir = ScratchDir("cancelled_save");
  const std::string path = fs::Join(dir, "flush.bin");
  // 3 MB at 30 ms per 1 MB write step: the stop lands mid-write.
  fs::SetFsFaults(fs::FsFaults{.write_step_delay_ms = 30});
  auto lane = StartLane(TestConfig());
  SaveRequest save;
  save.path = path;
  save.revision = 1;
  save.durable = false;
  save.bytes = PatternBytes(std::size_t{3} << 20, 5);
  SubmitSave(*lane, std::move(save));
  // Wait for the temporary file: the save is writing.
  bool writing = false;
  const std::uint64_t start = NowNs();
  while (!writing && NowNs() - start < 10'000'000'000ULL) {
    auto entries = fs::ListDirectory(dir);
    REQUIRE(entries.has_value());
    writing = !entries->empty();
    if (!writing) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(writing);
  lane->Stop();
  fs::ClearFsFaults();
  REQUIRE(lane->Stats().cancelled == 1);
  // Neither the target nor the temporary file is there.
  auto entries = fs::ListDirectory(dir);
  REQUIRE(entries.has_value());
  std::string left;
  for (const std::string& entry : *entries) left += entry + " ";
  INFO("left: " << left);
  CHECK(entries->empty());
}

TEST_CASE("A sharing violation on rename retries for up to 2 s",
          "[lane][sharing]") {
  if constexpr (!fs::kFsFaultHooks) {
    SKIP("the se::fs fault injector is compiled out of Release builds");
  }
  const MainThreadScope main;
  const std::string dir = ScratchDir("sharing");
  const std::string path = fs::Join(dir, "doc.json");
  REQUIRE(WriteBytes(path, TextBytes("old")));
  auto lane = StartLane(TestConfig());

  SECTION("another process lets go after 400 ms: the save succeeds") {
    fs::SetFsFaults(fs::FsFaults{.rename_busy_ms = 400});
    SaveRequest save;
    save.path = path;
    save.revision = 1;
    save.durable = true;
    save.bytes = TextBytes("new");
    const std::uint64_t start = NowNs();
    SubmitSave(*lane, std::move(save));
    std::vector<LaneCompletion> done;
    REQUIRE(DrainCount(*lane, done, 1));
    const std::uint64_t took = NowNs() - start;
    fs::ClearFsFaults();
    REQUIRE(done[0].result.has_value());
    const auto& outcome = std::get<SaveOutcome>(*done[0].result);
    REQUIRE(outcome.busy_retries > 5);
    REQUIRE(took >= 400'000'000ULL);
    REQUIRE(took < 2'000'000'000ULL);
    REQUIRE(testing::BytesText(ReadBytes(path)) == "new");
    REQUIRE(lane->DurableFailures() == 0);
  }
  SECTION("it holds the file for 3 s: the save fails after 2 s, cleanly") {
    fs::SetFsFaults(fs::FsFaults{.rename_busy_ms = 3000});
    LaneQuitProvider provider(*lane);
    QuitController quit;
    quit.AddProvider(&provider);
    quit.Request(QuitReason::kRequested);
    SaveRequest save;
    save.path = path;
    save.revision = 1;
    save.durable = true;
    save.bytes = TextBytes("new");
    const std::uint64_t start = NowNs();
    SubmitSave(*lane, std::move(save));
    std::vector<LaneCompletion> done;
    bool cancelled = false;
    REQUIRE(DrainUntil(*lane, done, [&] {
      AssertMainThread();
      quit.Poll();
      if (!quit.Pending()) cancelled = true;
      return !done.empty();
    }));
    const std::uint64_t took = NowNs() - start;
    fs::ClearFsFaults();
    REQUIRE_FALSE(done[0].result.has_value());
    CHECK(done[0].result.error().message.find("sharing violation") !=
          std::string::npos);
    CHECK(done[0].result.error().path == path);
    REQUIRE(took >= 2'000'000'000ULL);
    REQUIRE(took < 2'600'000'000ULL);
    // The document stays as it was, no temporary file remains, and the
    // failed Durable save called the quit off.
    REQUIRE(testing::BytesText(ReadBytes(path)) == "old");
    auto entries = fs::ListDirectory(dir);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 1);
    REQUIRE(lane->DurableFailures() == 1);
    quit.Poll();
    CHECK((cancelled || !quit.Pending()));
    REQUIRE(quit.Cancellations() == 1);
    CHECK_FALSE(quit.ReadyToExit());
  }
}

#if defined(_WIN32)
TEST_CASE("A reader without FILE_SHARE_DELETE makes the rename retry",
          "[lane][sharing]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("sharing_real");
  const std::string path = fs::Join(dir, "doc.json");
  REQUIRE(WriteBytes(path, TextBytes("old")));
  auto lane = StartLane(TestConfig());
  // SDL's IOStream opens with FILE_SHARE_READ only: another program
  // reading the file the way SDL does.
  SDL_IOStream* holder = SDL_IOFromFile(path.c_str(), "rb");
  REQUIRE(holder != nullptr);
  std::atomic<bool> released{false};
  std::jthread release([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    SDL_CloseIO(holder);
    released.store(true);
  });
  SaveRequest save;
  save.path = path;
  save.revision = 1;
  save.bytes = TextBytes("new");
  SubmitSave(*lane, std::move(save));
  std::vector<LaneCompletion> done;
  REQUIRE(DrainCount(*lane, done, 1));
  release.join();
  REQUIRE(done[0].result.has_value());
  CHECK(std::get<SaveOutcome>(*done[0].result).busy_retries > 0);
  REQUIRE(testing::BytesText(ReadBytes(path)) == "new");
}
#endif

TEST_CASE("Engine reads let another writer rename over the file",
          "[lane][sharing]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("share_delete");
  const std::string path = fs::Join(dir, "texture.png");
  const std::string next = fs::Join(dir, "texture.png.new");
  REQUIRE(WriteBytes(path, TextBytes("first")));
  REQUIRE(WriteBytes(next, TextBytes("second")));
  auto reader = fs::FileReader::Open(path);
  REQUIRE(reader.has_value());
  // An editor saving over the file the engine has open.
  auto replaced = fs::TryReplace(next, path);
  REQUIRE(replaced.has_value());
  CHECK(*replaced == fs::ReplaceOutcome::kReplaced);
  // The open reader still reads what it opened.
  std::vector<std::byte> bytes(16);
  auto got = reader->Read(bytes);
  REQUIRE(got.has_value());
  REQUIRE(testing::BytesText(std::span(bytes).first(*got)) == "first");
  reader->Close();
  REQUIRE(testing::BytesText(ReadBytes(path)) == "second");
}

TEST_CASE("The scan reports a changed watched file", "[lane][scan]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("scan");
  const std::string path = fs::Join(dir, "watched.txt");
  REQUIRE(WriteBytes(path, TextBytes("one")));
  LaneConfig config = TestConfig();
  config.scan_period_ns = 50'000'000;
  auto lane = StartLane(config);
  lane->Submit(Desc(JobClass::kBackground), MakeWatchJob(path, 42, true));
  std::vector<LaneCompletion> done;
  REQUIRE(DrainCount(*lane, done, 1));
  // Two scans without a change report nothing.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  lane->DrainCompletions(
      [&](LaneCompletion&& c) { done.push_back(std::move(c)); });
  REQUIRE(done.size() == 1);
  REQUIRE(WriteBytes(path, TextBytes("two, longer")));
  REQUIRE(DrainCount(*lane, done, 2, std::chrono::seconds(5)));
  REQUIRE(done[1].result.has_value());
  const auto* change = std::get_if<FileChange>(&*done[1].result);
  REQUIRE(change != nullptr);
  REQUIRE(change->tag == 42);
  CHECK(change->exists);
  REQUIRE(change->size == 11);
  REQUIRE(lane->Stats().scans >= 3);
  // Unwatched: no more reports.
  lane->Submit(Desc(JobClass::kBackground), MakeWatchJob(path, 42, false));
  REQUIRE(DrainCount(*lane, done, 3));
  REQUIRE(WriteBytes(path, TextBytes("three")));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  lane->DrainCompletions(
      [&](LaneCompletion&& c) { done.push_back(std::move(c)); });
  REQUIRE(done.size() == 3);
}

TEST_CASE("The scan reports a file being written once, when it settles",
          "[lane][scan]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("scan_settle");
  const std::string path = fs::Join(dir, "growing.txt");
  REQUIRE(WriteBytes(path, TextBytes("start")));
  LaneConfig config = TestConfig();
  config.scan_period_ns = 100'000'000;
  auto lane = StartLane(config);
  lane->Submit(Desc(JobClass::kBackground), MakeWatchJob(path, 7, true));
  std::vector<LaneCompletion> done;
  REQUIRE(DrainCount(*lane, done, 1));
  // A writer appends every 25 ms for 400 ms, so every scan while it runs
  // sees a new size. The rule before ADR-094 reported each of them.
  {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    REQUIRE(out);
    for (int i = 0; i < 16; ++i) {
      out << "0123456789";
      out.flush();
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
  REQUIRE(DrainCount(*lane, done, 2, std::chrono::seconds(5)));
  // Later scans of the settled file report nothing more.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  lane->DrainCompletions(
      [&](LaneCompletion&& c) { done.push_back(std::move(c)); });
  std::vector<std::uint64_t> sizes;
  for (std::size_t i = 1; i < done.size(); ++i) {
    REQUIRE(done[i].result.has_value());
    const auto* change = std::get_if<FileChange>(&*done[i].result);
    REQUIRE(change != nullptr);
    sizes.push_back(change->size);
  }
  std::string seen;
  for (const std::uint64_t size : sizes) seen += std::format(" {}", size);
  INFO("reported sizes:" << seen);
  // One report of the finished file; a second only if the writer stalled
  // for a whole scan period (a loaded machine).
  REQUIRE(sizes.size() <= 2);
  REQUIRE(sizes.back() == 5 + 16 * 10);
}

TEST_CASE("Replay and SessionLog flushes go through the lane",
          "[lane][flush]") {
  const MainThreadScope main;
  const std::string dir = ScratchDir("flush");
  const std::string replay_path = fs::Join(dir, "run.sereplay");
  const std::string log_path = fs::Join(dir, "run.selog");
  ReplayRecorder recorder;
  recorder.Begin(MakeReplayHeader({}, 1, 120), {});
  for (Tick tick = 1; tick <= 50; ++tick) {
    InputFrame frame;
    frame.tick = tick;
    recorder.Record(frame);
    recorder.RecordHash(tick, tick * 31);
  }
  SessionLog log;
  auto lane = StartLane(TestConfig());
  const Ticket replay = SubmitFlush(*lane, replay_path, recorder.Finish());
  const Ticket session = SubmitFlush(*lane, log_path, log.Write());
  std::vector<LaneCompletion> done;
  REQUIRE(DrainCount(*lane, done, 2));
  lane->Stop();
  for (const LaneCompletion& completion : done) {
    CHECK((completion.ticket == replay || completion.ticket == session));
    CHECK(completion.desc.job_class == JobClass::kBackground);
    CHECK_FALSE(completion.desc.durable);
    REQUIRE(completion.result.has_value());
  }
  auto reader = ReplayReader::Open(ReadBytes(replay_path));
  REQUIRE(reader.has_value());
  REQUIRE(reader->Frames().size() == 50);
  CHECK(SessionLog::Read(ReadBytes(log_path)).has_value());
}

// M13 decision 5: the game build logs a Durable save that fails during its
// quit and quits anyway; the editor's provider (the default) cancels.
TEST_CASE("A Durable failure does not cancel a game's quit",
          "[lane][sharing]") {
  if constexpr (!fs::kFsFaultHooks) {
    SKIP("the se::fs fault injector is compiled out of Release builds");
  }
  const MainThreadScope main;
  const std::string dir = ScratchDir("game_quit");
  const std::string path = fs::Join(dir, "run.sereplay");
  auto lane = StartLane(TestConfig());
  fs::SetFsFaults(fs::FsFaults{.rename_busy_ms = 2500});
  LaneQuitProvider provider(*lane);
  provider.SetCancelOnFailure(false);
  QuitController quit;
  quit.AddProvider(&provider);
  quit.Request(QuitReason::kRequested);
  SaveRequest save;
  save.path = path;
  save.revision = 1;
  save.durable = true;
  save.bytes = TextBytes("replay");
  SubmitSave(*lane, std::move(save));
  std::vector<LaneCompletion> done;
  REQUIRE(DrainUntil(*lane, done, [&] {
    AssertMainThread();
    quit.Poll();
    return !done.empty() && quit.ReadyToExit();
  }));
  fs::ClearFsFaults();
  REQUIRE_FALSE(done[0].result.has_value());
  REQUIRE(lane->DurableFailures() == 1);
  REQUIRE(quit.Cancellations() == 0);
  CHECK(quit.ReadyToExit());
  CHECK_FALSE(fs::Exists(path));
}

}  // namespace
}  // namespace se
