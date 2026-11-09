// Copyright 2026 The simple-engine-2 Authors.
//
// The SDL callbacks and the App, headless (no window, no GPU device), so
// these run everywhere, under TSan and ASan in WSL too: AppQuit(NULL),
// throws injected in AppIterate and AppEvent, throws from game code during
// shutdown, a failure at every boot step, the two-phase quit with
// providers, and immediate events from an engine thread through the inbox.

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "platform/support/test_app.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_runtime/app.h"
#include "se_runtime/app_config.h"
#include "se_runtime/integration.h"
#include "se_runtime/lifecycle.h"
#include "se_runtime/quit.h"
#include "se_runtime/sdl_app.h"
#include "support/test_roles.h"

namespace se {
namespace {

using testing::RunTestApp;
using testing::TestGameSetup;

std::vector<std::string>& LogLines() {
  static auto* const kLines = new std::vector<std::string>();
  return *kLines;
}

void CaptureLog(LogLevel /*level*/, std::string_view line) {
  LogLines().emplace_back(line);
}

bool Logged(std::string_view text) {
  for (const std::string& line : LogLines()) {
    if (line.find(text) != std::string::npos) return true;
  }
  return false;
}

// Captures the log for one test.
class LogCapture {
 public:
  LogCapture() {
    LogLines().clear();
    SetLogSink(&CaptureLog);
  }
  ~LogCapture() { SetLogSink(nullptr); }
  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;
};

// What a run did, from the hooks.
struct Observed {
  int boots = 0;
  int shutdowns = 0;
  SDL_AppResult shutdown_result = SDL_APP_CONTINUE;
  std::uint64_t iterates_at_shutdown = 0;
  std::vector<FrameReport> frames;
};

GameSetup Observe(Observed& observed) {
  GameSetup setup = TestGameSetup();
  setup.on_boot = [&observed](App& /*app*/) { ++observed.boots; };
  setup.on_frame = [&observed](App& /*app*/, const FrameReport& report) {
    observed.frames.push_back(report);
  };
  setup.on_shutdown = [&observed](App& app, SDL_AppResult result) {
    ++observed.shutdowns;
    observed.shutdown_result = result;
    observed.iterates_at_shutdown = app.Iterates();
  };
  return setup;
}

TEST_CASE("SDL_AppQuit accepts a NULL app state", "[callbacks]") {
  const testing::MainThreadScope main;
  const LogCapture log;
  SetTerminateOnQuit(false);
  SdlAppQuit(nullptr, SDL_APP_FAILURE);
  CHECK(Logged("SDL_AppQuit without an app"));
  CHECK(Logged("0 engine threads running"));
}

TEST_CASE("A headless app runs its frames and ticks, then quits",
          "[callbacks]") {
  Observed observed;
  const int result = RunTestApp(
      Observe(observed),
      {"--headless", "--frames", "30", "--fixed-frame-dt", "16.6667"});
  REQUIRE(result == 0);
  REQUIRE(observed.boots == 1);
  REQUIRE(observed.shutdowns == 1);
  CHECK(observed.shutdown_result == SDL_APP_SUCCESS);
  // 30 frames, then the iterate that sees the drained quit returns
  // SUCCESS.
  REQUIRE(observed.frames.size() == 30);
  REQUIRE(observed.iterates_at_shutdown == 31);
  // 60 Hz frames at 120 Hz ticks: exactly 2 ticks per frame.
  for (const FrameReport& frame : observed.frames) {
    REQUIRE(frame.plan.steps == 2);
    CHECK_FALSE(frame.rendered);
  }
  REQUIRE(observed.frames.back().next_tick == 61);
  // The quit is requested after the 30th frame's report.
  CHECK(observed.frames.back().state == LifecycleState::kRunning);
}

TEST_CASE("A throw in SDL_AppIterate reaches the catch-all and quits",
          "[callbacks]") {
  if (!kRuntimeTestHooks) SKIP("test hooks are not built into Release");
  const LogCapture log;
  Observed observed;
  const int result = RunTestApp(Observe(observed),
                                {"--headless", "--inject-throw", "iterate:3"});
  REQUIRE(result == 1);
  REQUIRE(observed.shutdowns == 1);
  CHECK(observed.shutdown_result == SDL_APP_FAILURE);
  REQUIRE(observed.iterates_at_shutdown == 3);
  REQUIRE(observed.frames.size() == 2);
  CHECK(Logged("fault in SDL_AppIterate"));
}

TEST_CASE("A throw in SDL_AppEvent reaches the catch-all and quits",
          "[callbacks]") {
  if (!kRuntimeTestHooks) SKIP("test hooks are not built into Release");
  const LogCapture log;
  Observed observed;
  GameSetup setup = Observe(observed);
  // Make sure an event arrives.
  setup.on_frame = [&observed](App& /*app*/, const FrameReport& report) {
    observed.frames.push_back(report);
    SDL_Event event{};
    event.type = SDL_EVENT_USER;
    SDL_PushEvent(&event);
  };
  const int result =
      RunTestApp(std::move(setup), {"--headless", "--inject-throw", "event:1"});
  REQUIRE(result == 1);
  REQUIRE(observed.shutdowns == 1);
  CHECK(observed.shutdown_result == SDL_APP_FAILURE);
  CHECK(Logged("fault in SDL_AppEvent"));
}

// A throw from game code while the App shuts down is contained: the
// remaining steps run, so the log worker writes the fault and stops and no
// engine thread is left (before ADR-044, ~App ran after main's role had
// ended and aborted on its main-thread check).
void RequireCleanShutdownAfterFault(std::string_view where) {
  CHECK(Logged("fault in " + std::string(where)));
  CHECK(Logged("test fault"));
  CHECK(Logged("shutdown (success)"));
  REQUIRE(integration::EngineThreadsRunning() == 0);
  REQUIRE(SDL_WasInit(0) == 0);
}

TEST_CASE("A throw from GameSetup::on_shutdown is contained", "[callbacks]") {
  const LogCapture log;
  int shutdowns = 0;
  GameSetup setup = TestGameSetup();
  setup.on_shutdown = [&shutdowns](App& /*app*/, SDL_AppResult /*result*/) {
    ++shutdowns;
    throw std::runtime_error("on_shutdown test fault");
  };
  // SDL's exit code comes from the last iterate (SUCCESS); Release builds
  // that end the process themselves exit with 1 after a shutdown fault.
  REQUIRE(RunTestApp(std::move(setup), {"--headless", "--frames", "3"}) == 0);
  REQUIRE(shutdowns == 1);
  RequireCleanShutdownAfterFault("GameSetup::on_shutdown");
}

TEST_CASE("A throw from GameState::OnExit at shutdown is contained",
          "[callbacks]") {
  const LogCapture log;
  testing::SetPlayExitThrows(true);
  const int result =
      RunTestApp(TestGameSetup(), {"--headless", "--frames", "3"});
  testing::SetPlayExitThrows(false);
  REQUIRE(result == 0);
  RequireCleanShutdownAfterFault("GameState::OnExit");
}

TEST_CASE("A failure at each boot step leaves nothing behind", "[callbacks]") {
  if (!kRuntimeTestHooks) SKIP("test hooks are not built into Release");
  for (std::size_t step = 1; step <= InitStepNames().size(); ++step) {
    INFO("boot step " << step << " (" << InitStepNames()[step - 1] << ")");
    const LogCapture log;
    Observed observed;
    const int result =
        RunTestApp(Observe(observed),
                   {"--headless", "--fail-init-step", std::to_string(step)});
    REQUIRE(result == 1);
    REQUIRE(observed.boots == 0);
    REQUIRE(observed.shutdowns == 0);
    CHECK(observed.frames.empty());
    CHECK(Logged("injected failure at boot step " + std::to_string(step)));
    CHECK(Logged("SDL_AppQuit without an app"));
    REQUIRE(integration::EngineThreadsRunning() == 0);
    REQUIRE(SDL_WasInit(0) == 0);
  }
}

// Keeps the quit pending for |pending| polls, then ready or cancelled.
class CountingProvider final : public QuitProvider {
 public:
  CountingProvider(int pending, QuitPoll then)
      : pending_(pending), then_(then) {}
  QuitPoll PollQuit(QuitReason /*reason*/) override {
    ++polls;
    if (pending_ > 0) {
      --pending_;
      return QuitPoll::kPending;
    }
    const QuitPoll result = then_;
    then_ = QuitPoll::kReady;
    return result;
  }
  [[nodiscard]] std::string_view Name() const override { return "counting"; }

  int polls = 0;

 private:
  int pending_;
  QuitPoll then_;
};

TEST_CASE("Callbacks keep iterating while a provider drains the quit",
          "[callbacks]") {
  Observed observed;
  CountingProvider provider(10, QuitPoll::kReady);
  GameSetup setup = Observe(observed);
  setup.quit_providers.push_back(&provider);
  setup.on_frame = [&observed](App& app, const FrameReport& report) {
    AssertMainThread();
    observed.frames.push_back(report);
    if (report.iterate == 5) app.RequestQuit();
  };
  REQUIRE(RunTestApp(std::move(setup), {"--headless"}) == 0);
  // Asked once per iterate from the sixth on: ten pending answers, then
  // ready, and the iterate that sees it returns SUCCESS.
  REQUIRE(provider.polls == 11);
  REQUIRE(observed.iterates_at_shutdown == 16);
  int pending_frames = 0;
  for (const FrameReport& frame : observed.frames) {
    if (frame.state == LifecycleState::kQuitPending) ++pending_frames;
  }
  REQUIRE(pending_frames == 10);
}

TEST_CASE("A provider cancels the quit and the app runs on", "[callbacks]") {
  Observed observed;
  CountingProvider provider(2, QuitPoll::kCancel);
  GameSetup setup = Observe(observed);
  setup.quit_providers.push_back(&provider);
  setup.on_frame = [&observed](App& app, const FrameReport& report) {
    AssertMainThread();
    observed.frames.push_back(report);
    if (report.iterate == 3 || report.iterate == 20) app.RequestQuit();
  };
  REQUIRE(RunTestApp(std::move(setup), {"--headless"}) == 0);
  REQUIRE(observed.iterates_at_shutdown == 21);
  REQUIRE(observed.frames.size() == 20);
  // Requested after iterate 3, pending in iterates 4 and 5, cancelled at
  // the start of iterate 6; requested again after iterate 20.
  CHECK(observed.frames[3].state == LifecycleState::kQuitPending);
  CHECK(observed.frames[4].state == LifecycleState::kQuitPending);
  CHECK(observed.frames[5].state == LifecycleState::kRunning);
  CHECK(observed.frames[19].state == LifecycleState::kRunning);
}

TEST_CASE("Immediate events from an engine thread reach the app via the inbox",
          "[callbacks]") {
  Observed observed;
  std::thread engine_thread;
  std::atomic<bool> pushed{false};
  std::vector<QuitReason> recovered;
  GameSetup setup = Observe(observed);
  setup.recovery = [&recovered](QuitReason reason) {
    recovered.push_back(reason);
  };
  std::uint64_t low_memory = 0;
  std::uint64_t inbox_events = 0;
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    observed.frames.push_back(report);
    if (report.iterate == 2) {
      engine_thread = std::thread([&pushed] {
        SDL_Event low{};
        low.type = SDL_EVENT_LOW_MEMORY;
        SDL_PushEvent(&low);
        SDL_Event terminating{};
        terminating.type = SDL_EVENT_TERMINATING;
        SDL_PushEvent(&terminating);
        pushed.store(true);
      });
    }
    low_memory = app.LowMemoryEvents();
    inbox_events = app.InboxEvents();
    if (report.iterate > 2000) app.RequestQuit();
  };
  setup.on_shutdown = [&](App& app, SDL_AppResult result) {
    observed.shutdown_result = result;
    low_memory = app.LowMemoryEvents();
    inbox_events = app.InboxEvents();
    if (engine_thread.joinable()) engine_thread.join();
  };
  REQUIRE(RunTestApp(std::move(setup), {"--headless"}) == 0);
  if (engine_thread.joinable()) engine_thread.join();
  CHECK(pushed.load());
  CHECK(observed.shutdown_result == SDL_APP_SUCCESS);
  REQUIRE(low_memory == 1);
  REQUIRE(inbox_events == 2);
  // TERMINATING ran the recovery hook and ended the app.
  REQUIRE(recovered == std::vector<QuitReason>{QuitReason::kTerminating});
  REQUIRE(observed.frames.size() < 2000);
}

}  // namespace
}  // namespace se
