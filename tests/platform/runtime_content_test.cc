// Copyright 2026 The simple-engine-2 Authors.
//
// The runtime side of M13a (docs/milestone-specs.md "M13a"; test names
// start with "runtime."): a headless App running the content test module
// (tests/support/content_test_module.h) over tests/fixtures/content. The
// start state waits at the content gate until its texts are resident, a
// changed text applies to the next state that loads it (kAssetSwap), focus
// loss is a logged GameState command a replay repeats, an empty stack ends
// the game, and a text that does not load is a state error.

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "platform/support/test_app.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_platform/fs.h"
#include "se_runtime/app.h"
#include "se_runtime/app_config.h"
#include "se_runtime/asset_runtime.h"
#include "se_runtime/quit.h"
#include "se_sim/content_bundle.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "support/content_test_module.h"
#include "support/module_session.h"
#include "support/test_roles.h"

#ifndef SE_TEST_OUT_DIR
#error "SE_TEST_OUT_DIR must name a writable directory"
#endif

namespace se {
namespace {

using testing::ContentFixtureRoot;
using testing::ContentStamp;
using testing::ContentStateText;
using testing::ContentTestDependencies;
using testing::ContentTestModule;
using testing::kContentPause;
using testing::kContentPlay;
using testing::kContentTitle;

// A content root of its own (a copy of tests/fixtures/content).
std::string CopyContent(const std::string& name) {
  const std::string root =
      fs::Join(fs::Join(SE_TEST_OUT_DIR, "content_roots"), name);
  const std::string from = fs::Join(ContentFixtureRoot(), "content");
  REQUIRE(fs::CreateDirectories(fs::Join(root, "content")).has_value());
  auto names = fs::ListDirectory(from);
  REQUIRE(names.has_value());
  for (const std::string& file : *names) {
    auto bytes = fs::ReadFile(fs::Join(from, file));
    REQUIRE(bytes.has_value());
    REQUIRE(fs::WriteFile(fs::Join(fs::Join(root, "content"), file), *bytes)
                .has_value());
  }
  return root;
}

// Ends a run that went wrong instead of waiting for CTest's timeout.
void QuitAfter(App& app, std::uint64_t start_ns, std::uint64_t limit_ns) {
  AssertMainThread();
  if (SDL_GetTicksNS() - start_ns > limit_ns) app.RequestQuit();
}

constexpr std::uint64_t kRunLimitNs = 30'000'000'000;

std::vector<std::byte> ReadBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(chars.size());
  std::ranges::transform(chars, bytes.begin(),
                         [](char c) { return static_cast<std::byte>(c); });
  return bytes;
}

GameSetup ContentSetup() {
  GameSetup setup = testing::TestGameSetup();
  setup.module = &ContentTestModule();
  setup.title = "runtime content test";
  setup.state_assets = [](std::string_view id) {
    StateAssets assets;
    assets.all_textures = false;
    assets.all_sounds = false;
    assets.texts = {std::string(ContentStateText(id))};
    return assets;
  };
  setup.text_dependencies = &ContentTestDependencies;
  return setup;
}

// The panel ring's lines, collected as the App runs.
struct LogLines {
  std::uint64_t serial = 0;
  std::vector<std::string> lines;

  void Take(App& app) {
    if (app.Logs() != nullptr) {
      app.Logs()->TryCopyPanelLinesSince(serial, lines);
    }
  }
  // At shutdown: the log worker delivers the last lines asynchronously.
  void Settle(App& app) {
    std::size_t quiet = 0;
    for (int i = 0; i < 50 && quiet < 3; ++i) {
      const std::size_t before = lines.size();
      Take(app);
      quiet = lines.size() == before ? quiet + 1 : 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  [[nodiscard]] std::ptrdiff_t Find(std::string_view text) const {
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].find(text) != std::string::npos) {
        return static_cast<std::ptrdiff_t>(i);
      }
    }
    return -1;
  }
};

// A key press and release with SDL_PushEvent (no real input), stamped now.
void PressKey(SDL_Keycode key) {
  for (const bool down : {true, false}) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.timestamp = SDL_GetTicksNS() + (down ? 0 : 1'000'000);
    event.key.key = key;
    event.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
    event.key.down = down;
    REQUIRE(SDL_PushEvent(&event));
  }
}

std::string_view TopOf(const App& app) {
  const GameStateStack* states = app.GameStates();
  return states != nullptr ? states->TopId() : std::string_view();
}

std::optional<std::uint64_t> TopStamp(const App& app) {
  const GameStateStack* states = app.GameStates();
  if (states == nullptr || states->Depth() == 0) return std::nullopt;
  const World* world = states->TopWorlds().front();
  for (const auto [e, stamp] :
       world->CReg().view<const ContentStamp>().each()) {
    return stamp.texts;
  }
  return std::nullopt;
}

ReplayReader OpenReplay(const std::string& path) {
  auto reader = ReplayReader::Open(ReadBytes(path));
  REQUIRE(reader.has_value());
  return std::move(*reader);
}

// The recorded session replayed headless from its content bundle.
testing::ModuleSessionResult Replay(const ReplayReader& reader)
    SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner) {
  REQUIRE(ContentBundle::IsBundle(reader.StartBytes()));
  auto bundle = ContentBundle::Read(reader.StartBytes());
  REQUIRE(bundle.has_value());
  ContentBundleSource source(std::move(*bundle));
  // Copied: RunModuleSession's input is read as the ticks run.
  ReplayReader input = reader;
  testing::ModuleSessionOptions options;
  options.module = &ContentTestModule();
  options.start_state = source.Bundle().StartState();
  options.bundle = &source;
  options.seed = reader.Header().seed;
  options.tick_rate = reader.Header().tick_rate;
  options.ticks = reader.Frames().size();
  options.input = &input;
  const std::span<const std::byte> session = reader.SessionBytes();
  options.session.assign(session.begin(), session.end());
  return testing::RunModuleSession(options);
}

TEST_CASE("The start state is created once its texts are resident",
          "[content]") {
  // The slow disk is --fs-faults, a test hook Release builds lack; without
  // it the title's text may land before the first frame.
  if (!kRuntimeTestHooks) SKIP("--fs-faults needs the runtime's test hooks");
  struct Frame {
    std::size_t depth = 0;
    Tick next_tick = 0;
    int steps = 0;
  };
  std::vector<Frame> frames;
  LogLines log;
  GameSetup setup = ContentSetup();
  const std::uint64_t start_ns = SDL_GetTicksNS();
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    frames.push_back(
        {app.GameStates()->Depth(), report.next_tick, report.plan.steps});
    log.Take(app);
    QuitAfter(app, start_ns, kRunLimitNs);
    // Forty ticks after the title began.
    if (report.next_tick > 40) app.RequestQuit();
  };
  std::vector<std::string> texts;
  setup.on_shutdown = [&](App& app, SDL_AppResult /*result*/) {
    log.Settle(app);
    for (const ContentText& text : app.Assets()->LoadedTexts().Texts()) {
      texts.push_back(text.path);
    }
  };
  // A slow disk (every read step of the lane waits 40 ms): the title waits
  // at the gate for some frames.
  REQUIRE(testing::RunTestApp(
              std::move(setup),
              {"--headless", "--frames", "100000000", "--fixed-frame-dt",
               "8.3333", "--fs-faults", "read-ms=40", "--content-root",
               ContentFixtureRoot()}) == 0);
  REQUIRE(frames.size() >= 40);
  REQUIRE(frames.back().next_tick > 40);
  // The title waited at the gate for at least one frame, and no tick ran
  // while it did.
  const auto created = std::ranges::find_if(
      frames, [](const Frame& frame) { return frame.depth == 1; });
  REQUIRE(created != frames.end());
  REQUIRE(created != frames.begin());
  for (auto it = frames.begin(); it != created; ++it) {
    REQUIRE(it->depth == 0);
    REQUIRE(it->next_tick == 1);
    REQUIRE(it->steps == 0);
  }
  REQUIRE(frames.back().next_tick > 1);
  REQUIRE(log.Find("start state 'title' pushed at boot") >= 0);
  const std::ptrdiff_t held = log.Find("waits at the content gate");
  const std::ptrdiff_t resident =
      log.Find("state 'title': its 1 assets are resident after");
  REQUIRE(held >= 0);
  CHECK(resident > held);
  CHECK(texts == std::vector<std::string>{"content/title.txt"});
}

TEST_CASE("A changed scene applies to the next state that loads it",
          "[content]") {
  const std::string root = CopyContent("changed_scene");
  const std::string record = std::string(SE_TEST_OUT_DIR) + "/changed.sereplay";
  const std::string changed =
      "entity banner 0 0 0 0\nentity star 3 4 0.5 0.5\n";
  enum class Step {
    kWaitTitle,
    kWaitReload,
    kWaitPlay,
    kWaitPause,
    kWaitTitleAgain,
    kWaitEnd
  };
  Step step = Step::kWaitTitle;
  std::uint64_t first_stamp = 0;
  std::uint64_t first_stamp_after_reload = 0;
  std::uint64_t second_stamp = 0;
  std::uint64_t reloads_before = 0;
  int frames_in_step = 0;
  LogLines log;
  const std::uint64_t start_ns = SDL_GetTicksNS();
  GameSetup setup = ContentSetup();
  setup.on_frame = [&](App& app, const FrameReport& /*report*/) {
    AssertMainThread();
    log.Take(app);
    QuitAfter(app, start_ns, kRunLimitNs);
    ++frames_in_step;
    const auto next = [&](Step to) {
      step = to;
      frames_in_step = 0;
    };
    const std::optional<std::uint64_t> stamp = TopStamp(app);
    switch (step) {
      case Step::kWaitTitle:
        if (TopOf(app) == kContentTitle && stamp) {
          first_stamp = *stamp;
          reloads_before = app.Assets()->Registry().Stats().reloads_resident;
          REQUIRE(fs::WriteFile(root + "/content/title.txt",
                                std::as_bytes(std::span(changed)))
                      .has_value());
          next(Step::kWaitReload);
        }
        break;
      case Step::kWaitReload:
        // The lane's scan saw the change and the new text landed; the
        // running title World is untouched.
        if (app.Assets()->Registry().Stats().reloads_resident >
            reloads_before) {
          first_stamp_after_reload = stamp.value_or(0);
          PressKey(SDLK_RETURN);
          next(Step::kWaitPlay);
        }
        break;
      case Step::kWaitPlay:
        if (TopOf(app) == kContentPlay && frames_in_step > 2) {
          PressKey(SDLK_ESCAPE);
          next(Step::kWaitPause);
        }
        break;
      case Step::kWaitPause:
        if (TopOf(app) == kContentPause && frames_in_step > 2) {
          PressKey(SDLK_Q);
          next(Step::kWaitTitleAgain);
        }
        break;
      case Step::kWaitTitleAgain:
        if (TopOf(app) == kContentTitle && stamp && frames_in_step > 2) {
          second_stamp = *stamp;
          PressKey(SDLK_Q);
          next(Step::kWaitEnd);
        }
        break;
      case Step::kWaitEnd:
        break;
    }
  };
  setup.on_shutdown = [&](App& app, SDL_AppResult /*result*/) {
    log.Settle(app);
  };
  REQUIRE(
      testing::RunTestApp(std::move(setup),
                          {"--headless", "--frames", "100000000",
                           "--fixed-frame-dt", "8.3333", "--content-root", root,
                           "--hot-reload", "on", "--record", record}) == 0);
  REQUIRE(step == Step::kWaitEnd);
  CHECK(first_stamp_after_reload == first_stamp);
  CHECK(second_stamp != first_stamp);
  REQUIRE(log.Find("a changed text applies to the next state that loads it") >=
          0);
  REQUIRE(log.Find("the game ended: no state left") >= 0);

  // The replay's log has the swap, before the tick the second title began
  // at, and the bundle both versions; it replays tick for tick.
  const ReplayReader reader = OpenReplay(record);
  auto session = SessionLog::Read(reader.SessionBytes());
  REQUIRE(session.has_value());
  const auto swap =
      std::ranges::find_if(session->Pending(), [](const SessionEntry& entry) {
        return entry.op == SessionOp::kAssetSwap;
      });
  REQUIRE(swap != session->Pending().end());
  CHECK(swap->target == ContentPathGuid("content/title.txt"));
  const testing::MainThreadScope main;
  const testing::ModuleSessionResult replayed = Replay(reader);
  REQUIRE(replayed.error.empty());
  REQUIRE(replayed.swaps_selected == 1);
  REQUIRE(replayed.hashes.size() == reader.Hashes().size());
  for (std::size_t i = 0; i < replayed.hashes.size(); ++i) {
    INFO("tick " << i + 1);
    REQUIRE(replayed.hashes[i] == reader.Hashes()[i]);
  }
  CHECK(replayed.top_states[swap->apply_before - 1] == kContentTitle);
  CHECK(replayed.top_states[swap->apply_before - 2] == kContentPause);
}

TEST_CASE("Focus loss is a logged state command", "[content]") {
  const std::string record = std::string(SE_TEST_OUT_DIR) + "/focus.sereplay";
  Tick focus_tick = 0;
  int frames_running = 0;
  LogLines log;
  GameSetup setup = ContentSetup();
  setup.start_state = std::string(kContentPlay);
  setup.on_focus_lost =
      [](std::string_view top) -> std::optional<StateCommand> {
    if (top != kContentPlay) return std::nullopt;
    return StateCommand{StackOp::kPush, std::string(kContentPause)};
  };
  const std::uint64_t start_ns = SDL_GetTicksNS();
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    log.Take(app);
    QuitAfter(app, start_ns, kRunLimitNs);
    // Forty ticks of pause after the focus loss, then the quit.
    if (focus_tick > 0 && report.next_tick > focus_tick + 40) {
      app.RequestQuit();
    }
    if (TopOf(app) == kContentPlay && report.next_tick > 1) ++frames_running;
    if (frames_running == 20 && focus_tick == 0) {
      // A window focus event, pushed as SDL would deliver it (headless:
      // window 0 is the App's).
      SDL_Event event{};
      event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
      event.window.windowID = 0;
      REQUIRE(SDL_PushEvent(&event));
      focus_tick = report.next_tick;
    }
  };
  setup.on_shutdown = [&](App& app, SDL_AppResult /*result*/) {
    log.Settle(app);
  };
  REQUIRE(testing::RunTestApp(std::move(setup),
                              {"--headless", "--frames", "100000000",
                               "--fixed-frame-dt", "8.3333", "--content-root",
                               ContentFixtureRoot(), "--record", record}) == 0);
  REQUIRE(focus_tick > 1);
  REQUIRE(log.Find("focus lost on 'play': GameState command push 'pause' "
                   "logged before tick") >= 0);
  const ReplayReader reader = OpenReplay(record);
  auto session = SessionLog::Read(reader.SessionBytes());
  REQUIRE(session.has_value());
  REQUIRE(session->PendingCount() == 1);
  const SessionEntry& entry = session->Pending().front();
  CHECK(entry.op == SessionOp::kGameStateCommand);
  CHECK(entry.apply_before >= focus_tick);
  const testing::MainThreadScope main;
  const testing::ModuleSessionResult replayed = Replay(reader);
  REQUIRE(replayed.error.empty());
  REQUIRE(replayed.hashes.size() == reader.Hashes().size());
  REQUIRE(replayed.hashes.size() >= entry.apply_before);
  for (std::size_t i = 0; i < replayed.hashes.size(); ++i) {
    INFO("tick " << i + 1);
    REQUIRE(replayed.hashes[i] == reader.Hashes()[i]);
  }
  // The replay pauses at the tick the live session did.
  CHECK(replayed.top_states[entry.apply_before - 2] == kContentPlay);
  CHECK(replayed.top_states[entry.apply_before - 1] == kContentPause);
}

TEST_CASE("An empty stack quits the game", "[content]") {
  bool pressed = false;
  std::optional<QuitReason> reason;
  LogLines log;
  GameSetup setup = ContentSetup();
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    log.Take(app);
    if (!pressed && TopOf(app) == kContentTitle && report.next_tick > 3) {
      PressKey(SDLK_Q);
      pressed = true;
    }
  };
  setup.on_shutdown = [&](App& app, SDL_AppResult /*result*/) {
    reason = app.Quit().Reason();
    log.Settle(app);
  };
  // --frames is far away: the game ends by itself.
  REQUIRE(testing::RunTestApp(
              std::move(setup),
              {"--headless", "--frames", "100000", "--fixed-frame-dt", "8.3333",
               "--content-root", ContentFixtureRoot()}) == 0);
  REQUIRE(pressed);
  REQUIRE(reason.has_value());
  CHECK(*reason == QuitReason::kGameEnded);
  REQUIRE(log.Find("the game ended: no state left") >= 0);
}

TEST_CASE("A state whose text does not load takes the fault path",
          "[content]") {
  LogLines log;
  GameSetup setup = ContentSetup();
  setup.state_assets = [](std::string_view /*id*/) {
    StateAssets assets;
    assets.all_textures = false;
    assets.all_sounds = false;
    assets.texts = {"content/no_such_scene.txt"};
    return assets;
  };
  bool created = false;
  const std::uint64_t start_ns = SDL_GetTicksNS();
  setup.on_frame = [&](App& app, const FrameReport& /*report*/) {
    AssertMainThread();
    log.Take(app);
    QuitAfter(app, start_ns, kRunLimitNs);
    created = created || app.GameStates()->Depth() > 0;
  };
  setup.on_shutdown = [&](App& app, SDL_AppResult /*result*/) {
    log.Settle(app);
  };
  REQUIRE(testing::RunTestApp(
              std::move(setup),
              {"--headless", "--frames", "100000000", "--fixed-frame-dt",
               "8.3333", "--content-root", ContentFixtureRoot()}) == 1);
  CHECK_FALSE(created);
  REQUIRE(log.Find("state 'title' cannot be created") >= 0);
  REQUIRE(log.Find("no_such_scene.txt") >= 0);
}

// Review fix F2: a state whose text was read but whose content does not
// load (a line the module cannot parse) is not created either: the stack
// takes it off again (GameState::EnterError) and the App takes the fault
// path with the text's path and line, instead of running on an empty World.
TEST_CASE("A state whose content does not load takes the fault path",
          "[content]") {
  const std::string root = CopyContent("bad_content");
  {
    std::ofstream title(fs::Join(root, "content/title.txt"),
                        std::ios::binary | std::ios::trunc);
    title << "entity banner 0 0 0 0\nnot an entity\n";
  }
  LogLines log;
  GameSetup setup = ContentSetup();
  bool created = false;
  const std::uint64_t start_ns = SDL_GetTicksNS();
  setup.on_frame = [&](App& app, const FrameReport& /*report*/) {
    AssertMainThread();
    log.Take(app);
    QuitAfter(app, start_ns, kRunLimitNs);
    created = created || app.GameStates()->Depth() > 0;
  };
  setup.on_shutdown = [&](App& app, SDL_AppResult /*result*/) {
    log.Settle(app);
  };
  REQUIRE(testing::RunTestApp(
              std::move(setup),
              {"--headless", "--frames", "100000000", "--fixed-frame-dt",
               "8.3333", "--content-root", root}) == 1);
  CHECK_FALSE(created);
  REQUIRE(log.Find("state 'title' cannot be created") >= 0);
  REQUIRE(log.Find("content/title.txt:2:1") >= 0);
}

}  // namespace
}  // namespace se
