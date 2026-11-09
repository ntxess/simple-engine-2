// Copyright 2026 The simple-engine-2 Authors.
//
// The M8 input path without a window (label unit, everywhere, TSan and ASan
// in WSL): the settings reader, the ActionMap (config/config.json's legacy
// key names, default pad bindings, unknown names as errors) and the
// InputService's latching: order, held state, focus-loss clears, per-tick
// edges with the placement rule, the last-tick rule, frames without a tick,
// gamepad half axes and the axes.

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "game_rampage/module.h"
#include "se_core/input_frame.h"
#include "se_core/time.h"
#include "se_platform/fs.h"
#include "se_runtime/action_map.h"
#include "se_runtime/input_service.h"
#include "se_runtime/user_settings.h"
#include "se_sim/game_module.h"

#ifndef SE_REPO_ROOT
#error "SE_REPO_ROOT must name the repository"
#endif

namespace se {
namespace {

constexpr std::uint64_t kMs = 1'000'000;

// up, left, enter and back (enter and back share Enter), a jump on the
// pad's south button and the left stick's vertical halves.
constexpr std::array<ActionBinding, 5> kBindings = {{
    {"up", "w", "key-up", "dpup,-lefty"},
    {"left", "a", "key-left", "dpleft"},
    {"enter", "enter", "key-enter", "start"},
    {"back", "enter", "key-back", "east"},
    {"down", "s", "key-down", "+lefty"},
}};

ActionMap TestMap(const UserSettings& settings = {}) {
  auto map =
      ActionMap::Build(ActionMapDefaults{.bindings = kBindings}, settings);
  REQUIRE(map);
  return std::move(*map);
}

InputService TestService() {
  InputServiceConfig config;
  config.key_source = EdgeSource::kRawKeyboard;
  return InputService(TestMap(), config);
}

TEST_CASE("The settings file gives key bindings and volumes", "[settings]") {
  const std::string json = R"({
    "system": {"name": "x", "nested": [1, 2.5e3, true, null]},
    "sound-settings": {"music-volume": 40.0, "effects-volume": 140},
    "control-settings": {"key-bindings": {"key-up": "i", "key-down": "k"}}
  })";
  auto settings = ParseUserSettings(json, "settings.json");
  REQUIRE(settings);
  REQUIRE(settings->key_bindings.size() == 2);
  REQUIRE(settings->key_bindings[0].first == "key-up");
  REQUIRE(settings->key_bindings[0].second == "i");
  REQUIRE(settings->music_volume == 40.0);
  REQUIRE(settings->effects_volume == 100.0);  // clamped

  auto defaults = ParseUserSettings("{}", "empty.json");
  REQUIRE(defaults);
  REQUIRE(defaults->key_bindings.empty());
  REQUIRE(defaults->music_volume == 100.0);

  auto wrong = ParseUserSettings(
      R"({"control-settings": {"key-bindings": {"key-up": 3}}})", "w.json");
  REQUIRE_FALSE(wrong);
  REQUIRE(wrong.error().path == "w.json");
  REQUIRE(wrong.error().message.find("key-up") != std::string::npos);

  auto broken = ParseUserSettings("{\n  \"a\": [1, 2,]\n}", "b.json");
  REQUIRE_FALSE(broken);
  REQUIRE(broken.error().code == ErrorCode::kParse);
  REQUIRE(broken.error().line == 2);
  auto duplicate = ParseUserSettings(R"({"a": 1, "a": 2})", "d.json");
  REQUIRE_FALSE(duplicate);
  REQUIRE(duplicate.error().message.find("duplicate") != std::string::npos);
}

TEST_CASE("Name tables map names to paths", "[settings]") {
  auto table = ParseNameTable(R"({"hit": "assets/sounds/hit.wav", "xé": "b"})",
                              "sound.json");
  REQUIRE(table);
  REQUIRE(table->size() == 2);
  REQUIRE((*table)[0].second == "assets/sounds/hit.wav");
  REQUIRE((*table)[1].first == "x\xC3\xA9");
  CHECK_FALSE(ParseNameTable(R"({"hit": 1})", "sound.json"));
  CHECK_FALSE(ParseNameTable(R"(["a"])", "sound.json"));
}

TEST_CASE("The repository's config file builds rampage's action map",
          "[settings][action_map]") {
  auto settings =
      LoadUserSettings(std::string(SE_REPO_ROOT) + "/config/config.json");
  REQUIRE(settings);
  REQUIRE(settings->key_bindings.size() == 14);
  REQUIRE(settings->music_volume == 100.0);
  auto map = ActionMap::Build(rampage::RampageModule().actions, *settings);
  REQUIRE(map);
  REQUIRE(map->ActionCount() == 14);
  using rampage::Action;
  const auto bit = [](Action a) { return 1ULL << static_cast<int>(a); };
  REQUIRE(map->ActionsForKey(SDLK_W) == bit(Action::kUp));
  REQUIRE(map->ActionsForKey(SDLK_D) == bit(Action::kRight));
  REQUIRE(map->ActionsForKey(SDLK_SPACE) == bit(Action::kJump));
  REQUIRE(map->ActionsForKey(SDLK_ESCAPE) == bit(Action::kExit));
  REQUIRE(map->ActionsForKey(SDLK_RETURN) ==
          (bit(Action::kEnter) | bit(Action::kBack)));
  REQUIRE(map->ActionsForKey(SDLK_L) == bit(Action::kA6));
  REQUIRE(map->ActionsForPadButton(SDL_GAMEPAD_BUTTON_DPAD_UP) ==
          bit(Action::kUp));
  REQUIRE(map->ActionsForPadButton(SDL_GAMEPAD_BUTTON_SOUTH) ==
          bit(Action::kJump));
  REQUIRE(map->ActionsForPadAxis(SDL_GAMEPAD_AXIS_LEFTY, false) ==
          bit(Action::kUp));
  REQUIRE(map->ActionsForPadAxis(SDL_GAMEPAD_AXIS_LEFTX, true) ==
          bit(Action::kRight));
  for (const ActionMap::Binding& binding : map->Bindings()) {
    REQUIRE(binding.from_settings);
  }
}

TEST_CASE("The action map resolves legacy and SDL names", "[action_map]") {
  REQUIRE(KeycodeFromName("w") == SDLK_W);
  REQUIRE(KeycodeFromName("W") == SDLK_W);
  REQUIRE(KeycodeFromName("7") == SDLK_7);
  REQUIRE(KeycodeFromName("esc") == SDLK_ESCAPE);
  REQUIRE(KeycodeFromName("Escape") == SDLK_ESCAPE);
  REQUIRE(KeycodeFromName("enter") == SDLK_RETURN);
  REQUIRE(KeycodeFromName("space") == SDLK_SPACE);
  REQUIRE(KeycodeFromName("up") == SDLK_UP);
  REQUIRE(KeycodeFromName("F5") == SDLK_F5);
  REQUIRE(KeycodeFromName("NoSuchKey") == SDLK_UNKNOWN);
  REQUIRE(KeycodeFromName("mouse-left") == SDLK_UNKNOWN);
  REQUIRE(MouseButtonFromName("mouse-left") == SDL_BUTTON_LEFT);
  REQUIRE(MouseButtonFromName("mouse-x2") == SDL_BUTTON_X2);

  const ActionMap map = TestMap();
  REQUIRE(map.ActionsForKey(SDLK_RETURN) == 0b1100);
  REQUIRE(map.ActionsForPadButton(SDL_GAMEPAD_BUTTON_START) == 0b0100);
  REQUIRE(map.ActionsForPadAxis(SDL_GAMEPAD_AXIS_LEFTY, true) == 0b10000);
  REQUIRE(map.AxisBound(SDL_GAMEPAD_AXIS_LEFTY));
  CHECK_FALSE(map.AxisBound(SDL_GAMEPAD_AXIS_RIGHTX));

  // A user binding replaces the default key.
  UserSettings settings;
  settings.key_bindings = {{"key-up", "i"}, {"key-left", "mouse-right"}};
  const ActionMap rebound = TestMap(settings);
  REQUIRE(rebound.ActionsForKey(SDLK_W) == 0);
  REQUIRE(rebound.ActionsForKey(SDLK_I) == 0b1);
  REQUIRE(rebound.ActionsForMouse(SDL_BUTTON_RIGHT) == 0b10);
}

TEST_CASE("Unknown names are boot errors naming the key", "[action_map]") {
  UserSettings settings;
  settings.key_bindings = {{"key-up", "nosuchkey"}};
  auto bad_key =
      ActionMap::Build(ActionMapDefaults{.bindings = kBindings}, settings);
  REQUIRE_FALSE(bad_key);
  REQUIRE(bad_key.error().message.find("nosuchkey") != std::string::npos);
  REQUIRE(bad_key.error().message.find("key-up") != std::string::npos);

  settings.key_bindings = {{"key-7", "u"}};
  auto undeclared =
      ActionMap::Build(ActionMapDefaults{.bindings = kBindings}, settings);
  REQUIRE_FALSE(undeclared);
  REQUIRE(undeclared.error().message.find("key-7") != std::string::npos);

  constexpr std::array<ActionBinding, 1> kBadPad = {
      {{"jump", "space", "", "south,wiggle"}}};
  auto bad_pad = ActionMap::Build(ActionMapDefaults{.bindings = kBadPad}, {});
  REQUIRE_FALSE(bad_pad);
  REQUIRE(bad_pad.error().message.find("wiggle") != std::string::npos);
}

TEST_CASE("Edges latch in stamp order with their sources", "[input_service]") {
  InputService input = TestService();
  // Arrival order differs from stamp order: the stamps decide.
  input.OnKey(SDLK_A, true, false, 30 * kMs);
  input.OnKey(SDLK_W, true, false, 10 * kMs);
  input.OnKey(SDLK_W, true, true, 12 * kMs);  // a repeat: ignored
  input.OnKey(SDLK_W, false, false, 20 * kMs);
  const InputFrame frame = input.Latch(7, 100 * kMs, true);
  REQUIRE(frame.tick == 7);
  REQUIRE(frame.platform_live);
  REQUIRE(frame.edge_count == 3);
  REQUIRE(frame.edges[0] ==
          InputEdge{0, true, 10 * kMs, 0, EdgeSource::kRawKeyboard});
  REQUIRE(frame.edges[1] ==
          InputEdge{0, false, 20 * kMs, 0, EdgeSource::kRawKeyboard});
  REQUIRE(frame.edges[2] ==
          InputEdge{1, true, 30 * kMs, 0, EdgeSource::kRawKeyboard});
  REQUIRE(frame.held == 0b10);
  REQUIRE(frame.pressed[0] == 1);
  REQUIRE(frame.released[0] == 1);
  REQUIRE(frame.pressed[1] == 1);
  // Held state carries over; no new edges.
  const InputFrame next = input.Latch(8, 110 * kMs, true);
  REQUIRE(next.held == 0b10);
  REQUIRE(next.edge_count == 0);
}

TEST_CASE("Each tick takes the edges stamped before its window end",
          "[input_service]") {
  InputService input = TestService();
  // Two ticks of a 60 Hz Locked frame: windows end at 8.33 and 16.67 ms.
  const std::uint64_t first_end = 8'333'333;
  const std::uint64_t second_end = 16'666'667;
  input.OnKey(SDLK_W, true, false, 1 * kMs);
  input.OnKey(SDLK_A, true, false, 9 * kMs);
  // After the last window: the last tick takes it.
  input.OnKey(SDLK_S, true, false, 18 * kMs);
  const InputFrame t1 = input.Latch(1, first_end, false);
  const InputFrame t2 = input.Latch(2, second_end, true);
  REQUIRE(t1.edge_count == 1);
  REQUIRE(t1.edges[0].action == 0);
  REQUIRE(t1.held == 0b1);
  REQUIRE(t2.edge_count == 2);
  REQUIRE(t2.edges[0].action == 1);
  REQUIRE(t2.edges[1].action == 4);
  REQUIRE(t2.held == 0b10011);

  // A frame without a tick latches nothing: the edge waits for the next
  // frame's first tick.
  input.OnKey(SDLK_A, false, false, 20 * kMs);
  REQUIRE(input.Pending() == 1);
  const InputFrame t3 = input.Latch(3, 33'333'333, false);
  REQUIRE(t3.edge_count == 1);
  CHECK_FALSE(t3.edges[0].down);
  REQUIRE(t3.held == 0b10001);
  REQUIRE(input.Stats().late_changes == 0);

  // A change stamped before a window already latched is late but kept.
  input.OnKey(SDLK_W, false, false, 15 * kMs);
  const InputFrame t4 = input.Latch(4, 41'666'667, true);
  REQUIRE(t4.edge_count == 1);
  REQUIRE(input.Stats().late_changes == 1);
}

TEST_CASE("Shared and repeated inputs hold an action once", "[input_service]") {
  InputService input = TestService();
  // Enter holds enter and back; W and the pad's d-pad both hold up.
  input.OnKey(SDLK_RETURN, true, false, 1 * kMs);
  input.OnKey(SDLK_W, true, false, 2 * kMs);
  input.OnPadButton(5, SDL_GAMEPAD_BUTTON_DPAD_UP, true, 3 * kMs);
  InputFrame frame = input.Latch(1, 10 * kMs, true);
  REQUIRE(frame.edge_count == 3);
  REQUIRE(frame.edges[0].action == 2);
  REQUIRE(frame.edges[1].action == 3);
  REQUIRE(frame.edges[1].order == 1);  // same stamp as the first
  REQUIRE(frame.edges[2].action == 0);
  REQUIRE(frame.held == 0b1101);
  // Releasing one of the two holders of up keeps it held.
  input.OnKey(SDLK_W, false, false, 11 * kMs);
  frame = input.Latch(2, 20 * kMs, true);
  REQUIRE(frame.edge_count == 0);
  REQUIRE(frame.held == 0b1101);
  input.OnPadButton(5, SDL_GAMEPAD_BUTTON_DPAD_UP, false, 21 * kMs);
  frame = input.Latch(3, 30 * kMs, true);
  REQUIRE(frame.edge_count == 1);
  REQUIRE(frame.edges[0].source == EdgeSource::kGamepadSampler);
  REQUIRE(frame.held == 0b1100);
}

TEST_CASE("Focus loss clears held input with stamped releases",
          "[input_service]") {
  InputService input = TestService();
  input.OnKey(SDLK_W, true, false, 1 * kMs);
  input.OnPadButton(2, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true, 2 * kMs);
  input.OnPadAxis(2, SDL_GAMEPAD_AXIS_RIGHTX, 0.75F, 3 * kMs);
  InputFrame frame = input.Latch(1, 10 * kMs, true);
  REQUIRE(frame.held == 0b11);
  REQUIRE(frame.axes[SDL_GAMEPAD_AXIS_RIGHTX] == 0.75F);
  input.ClearHeld(12 * kMs);
  frame = input.Latch(2, 20 * kMs, true);
  REQUIRE(frame.held == 0);
  REQUIRE(frame.edge_count == 2);
  for (int i = 0; i < 2; ++i) {
    CHECK_FALSE(frame.edges[i].down);
    REQUIRE(frame.edges[i].stamp_ns == 12 * kMs);
    REQUIRE(frame.edges[i].source == EdgeSource::kCleared);
  }
  REQUIRE(frame.axes[SDL_GAMEPAD_AXIS_RIGHTX] == 0.0F);
  REQUIRE(input.Stats().clears == 1);
  // The key's own release after the clear changes nothing.
  input.OnKey(SDLK_W, false, false, 21 * kMs);
  frame = input.Latch(3, 30 * kMs, true);
  REQUIRE(frame.edge_count == 0);
}

TEST_CASE("Stick halves press past a threshold with hysteresis",
          "[input_service]") {
  InputService input = TestService();
  input.OnPadAxis(1, SDL_GAMEPAD_AXIS_LEFTY, -0.3F, 1 * kMs);
  input.OnPadAxis(1, SDL_GAMEPAD_AXIS_LEFTY, -0.6F, 2 * kMs);  // up pressed
  input.OnPadAxis(1, SDL_GAMEPAD_AXIS_LEFTY, -0.4F, 3 * kMs);  // still held
  InputFrame frame = input.Latch(1, 10 * kMs, true);
  REQUIRE(frame.held == 0b1);
  REQUIRE(frame.axes[SDL_GAMEPAD_AXIS_LEFTY] == -0.4F);
  input.OnPadAxis(1, SDL_GAMEPAD_AXIS_LEFTY, 0.9F, 11 * kMs);  // up, down
  frame = input.Latch(2, 20 * kMs, true);
  REQUIRE(frame.edge_count == 2);
  CHECK_FALSE(frame.edges[0].down);
  REQUIRE(frame.edges[0].action == 0);
  REQUIRE(frame.edges[1].down);
  REQUIRE(frame.edges[1].action == 4);
  // The pad goes away: what it held is released.
  input.OnPadRemoved(1, 25 * kMs);
  frame = input.Latch(3, 30 * kMs, true);
  REQUIRE(frame.held == 0);
}

TEST_CASE("Axis samples follow the tick windows", "[input_service]") {
  InputService input = TestService();
  input.OnPadAxis(1, SDL_GAMEPAD_AXIS_RIGHTX, 0.25F, 1 * kMs);
  input.OnPadAxis(1, SDL_GAMEPAD_AXIS_RIGHTX, 0.5F, 9 * kMs);
  const InputFrame t1 = input.Latch(1, 8'333'333, false);
  const InputFrame t2 = input.Latch(2, 16'666'667, true);
  REQUIRE(t1.axes[SDL_GAMEPAD_AXIS_RIGHTX] == 0.25F);
  REQUIRE(t2.axes[SDL_GAMEPAD_AXIS_RIGHTX] == 0.5F);
}

TEST_CASE("Edges beyond the frame's capacity are counted", "[input_service]") {
  InputService input = TestService();
  for (std::uint64_t i = 0; i < 40; ++i) {
    input.OnKey(SDLK_W, i % 2 == 0, false, (i + 1) * kMs);
  }
  const InputFrame frame = input.Latch(1, 100 * kMs, true);
  REQUIRE(frame.edge_count == kMaxEdgesPerTick);
  REQUIRE(frame.pressed[0] == 20);
  REQUIRE(frame.released[0] == 20);
  REQUIRE(frame.held == 0);
  REQUIRE(input.Stats().edges_not_stored == 40 - kMaxEdgesPerTick);
}

TEST_CASE("Without latches the oldest changes fold into the held state",
          "[input_service]") {
  InputService input = TestService();
  const std::size_t n = InputService::kMaxPending + 11;
  for (std::size_t i = 0; i < n; ++i) {
    input.OnKey(SDLK_W, i % 2 == 0, false, (i + 1) * kMs);
  }
  REQUIRE(input.Pending() == InputService::kMaxPending);
  REQUIRE(input.Stats().changes_folded == 11);
  const InputFrame frame = input.Latch(1, (n + 5) * kMs, true);
  // The held state is right whatever folded: the last change was a press.
  REQUIRE(frame.held == 0b1);
}

TEST_CASE("SDL events reach the service with their timestamps",
          "[input_service]") {
  InputService input = TestService();
  SDL_Event key{};
  key.type = SDL_EVENT_KEY_DOWN;
  key.key.key = SDLK_W;
  key.key.down = true;
  key.key.timestamp = 5 * kMs;
  REQUIRE(input.HandleEvent(key));
  SDL_Event pad{};
  pad.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
  pad.gaxis.which = 3;
  pad.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTY;
  pad.gaxis.value = 32767;
  pad.gaxis.timestamp = 6 * kMs;
  REQUIRE(input.HandleEvent(pad));
  SDL_Event motion{};
  motion.type = SDL_EVENT_MOUSE_MOTION;
  CHECK_FALSE(input.HandleEvent(motion));
  const InputFrame frame = input.Latch(1, 10 * kMs, true);
  REQUIRE(frame.edge_count == 2);
  REQUIRE(frame.edges[0].stamp_ns == 5 * kMs);
  REQUIRE(frame.edges[1].stamp_ns == 6 * kMs);
  REQUIRE(frame.edges[1].source == EdgeSource::kGamepadSampler);
  REQUIRE(frame.axes[SDL_GAMEPAD_AXIS_LEFTY] == 1.0F);
}

TEST_CASE("At 60 Hz Locked, presses 8 ms apart land on different ticks",
          "[input_service]") {
  InputService input = TestService();
  FixedStepper stepper;
  FrameClock clock;
  clock.period_sec = 1.0 / 60.0;
  clock.vblanks_known = true;
  clock.elapsed_vblanks = 1;
  clock.frame_start_ns = 1'000'000'000;
  static_cast<void>(stepper.Plan(clock, FrameCosts{}, true, false, false));
  clock.prev_frame_start_ns = clock.frame_start_ns;
  clock.frame_start_ns += 16'666'667;
  // The events of the frame that just ended.
  input.OnKey(SDLK_W, true, false, clock.prev_frame_start_ns + 2 * kMs);
  input.OnKey(SDLK_A, true, false, clock.prev_frame_start_ns + 10 * kMs);
  const StepPlan plan = stepper.Plan(clock, FrameCosts{}, true, false, false);
  REQUIRE(plan.cadence == Cadence::kLocked);
  REQUIRE(plan.steps == 2);
  const InputFrame t1 = input.Latch(1, plan.tick_window_end_ns[0], false);
  const InputFrame t2 = input.Latch(2, plan.tick_window_end_ns[1], true);
  REQUIRE(t1.edge_count == 1);
  REQUIRE(t1.edges[0].action == 0);
  REQUIRE(t2.edge_count == 1);
  REQUIRE(t2.edges[0].action == 1);
}

}  // namespace
}  // namespace se
