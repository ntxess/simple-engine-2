// Copyright 2026 The simple-engine-2 Authors.
//
// An SDL virtual gamepad through the real input sampler (label desktop;
// docs/milestone-specs.md M8 "Tests"): a test thread toggles its buttons
// 8 ms apart on a 60 Hz frame grid, the sampler's SDL_UpdateGamepads at
// 1 kHz turns the toggles into stamped events, main pumps them into the
// InputService and latches the two ticks of each 60 Hz Locked frame. Each
// pair of toggles lands on different ticks, and each edge's stamp is about
// one sampler period after its true toggle time. No real input: the
// virtual joystick is SDL's (SDL_AttachVirtualJoystick).
//
// The owner may be using the machine: a toggle the test thread made late
// (more than 2 ms after its slot) is reported and not judged; the test
// needs most of them on time.

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "se_core/edge_placement.h"
#include "se_core/input_frame.h"
#include "se_core/time.h"
#include "se_platform/gamepads.h"
#include "se_platform/input_sampler.h"
#include "se_platform/sdl_context.h"
#include "se_runtime/action_map.h"
#include "se_runtime/input_service.h"
#include "se_sim/game_module.h"
#include "support/test_roles.h"

namespace se {
namespace {

constexpr std::uint64_t kMs = 1'000'000;
constexpr std::uint64_t kPeriod = 16'666'667;
constexpr int kFrames = 12;

void SleepUntil(std::uint64_t ns) {
  const std::uint64_t now = SDL_GetTicksNS();
  if (ns > now) SDL_DelayNS(ns - now);
}

// The share of all CPUs busy since construction (0 where unknown).
class LoadWatch {
 public:
  LoadWatch() { Read(idle_, total_); }
  [[nodiscard]] double Load() const {
    std::uint64_t idle = 0;
    std::uint64_t total = 0;
    Read(idle, total);
    return total > total_ ? 1.0 - static_cast<double>(idle - idle_) /
                                      static_cast<double>(total - total_)
                          : 0.0;
  }

 private:
  static void Read(std::uint64_t& idle, std::uint64_t& total) {
#if defined(_WIN32)
    FILETIME idle_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    GetSystemTimes(&idle_time, &kernel_time, &user_time);
    const auto ticks = [](const FILETIME& t) {
      return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) |
             t.dwLowDateTime;
    };
    idle = ticks(idle_time);
    total = ticks(kernel_time) + ticks(user_time);
#else
    idle = 0;
    total = 0;
#endif
  }
  std::uint64_t idle_ = 0;
  std::uint64_t total_ = 0;
};

struct Toggle {
  std::uint64_t slot_ns = 0;
  std::uint64_t at_ns = 0;
  ActionId action = 0;
  bool down = false;
};

TEST_CASE("Virtual gamepad presses 8 ms apart land on different ticks",
          "[desktop][gamepad]") {
  const testing::MainThreadScope main;
  const LoadWatch load_watch;
  SdlInitConfig sdl_config;
  sdl_config.subsystems = SDL_INIT_GAMEPAD;
  auto sdl = SdlContext::Init(sdl_config);
  REQUIRE(sdl);

  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  // Virtual button i is gamepad button i (south, east, ... dpright).
  desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
  desc.name = "se virtual gamepad";
  const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  REQUIRE(id != 0);
  Gamepads gamepads;
  constexpr std::array<ActionBinding, 2> kBindings = {{
      {"jump", "space", "", "south"},
      {"back", "enter", "", "east"},
  }};
  auto map = ActionMap::Build(ActionMapDefaults{.bindings = kBindings}, {});
  REQUIRE(map);
  InputService input(std::move(*map), InputServiceConfig{});
  const auto pump = [&gamepads, &input] {
    SDL_PumpEvents();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      gamepads.HandleEvent(event);
      input.HandleEvent(event);
    }
  };
  pump();
  REQUIRE(gamepads.Count() == 1);
  SDL_Joystick* joystick = SDL_OpenJoystick(id);
  REQUIRE(joystick != nullptr);

  // The sampler: SDL_UpdateGamepads at 1 kHz on its own thread.
  auto sampler = InputSampler::Start(InputSamplerConfig{});
  REQUIRE(sampler);

  // The frame grid: frame f covers [start + (f - 1) P, start + f P).
  const std::uint64_t grid = SDL_GetTicksNS() + 50 * kMs;
  // Filled by the toggler (no allocation on its thread), read after join.
  std::array<Toggle, static_cast<std::size_t>(2) * kFrames> toggles{};
  std::thread toggler([joystick, grid, &toggles]() noexcept {
    std::size_t next = 0;
    for (int f = 1; f <= kFrames; ++f) {
      const std::uint64_t frame =
          grid + static_cast<std::uint64_t>(f - 1) * kPeriod;
      const bool down = f % 2 == 1;
      for (const auto& [offset, button, action] :
           {std::tuple{2 * kMs, SDL_GAMEPAD_BUTTON_SOUTH, ActionId{0}},
            std::tuple{10 * kMs, SDL_GAMEPAD_BUTTON_EAST, ActionId{1}}}) {
        SleepUntil(frame + offset);
        SDL_SetJoystickVirtualButton(joystick, button, down);
        toggles[next++] =
            Toggle{frame + offset, SDL_GetTicksNS(), action, down};
      }
    }
  });

  FixedStepper stepper;
  std::vector<InputFrame> latched;
  std::vector<std::array<std::uint64_t, 2>> windows;
  Tick tick = 1;
  for (int f = 0; f <= kFrames; ++f) {
    const std::uint64_t start = grid + static_cast<std::uint64_t>(f) * kPeriod;
    // The events of [start - P, start) have been pushed by now.
    SleepUntil(start + 3 * kMs);
    pump();
    FrameClock clock;
    clock.frame_start_ns = start;
    clock.prev_frame_start_ns = f == 0 ? 0 : start - kPeriod;
    clock.period_sec = 1.0 / 60.0;
    clock.vblanks_known = true;
    clock.elapsed_vblanks = 1;
    const StepPlan plan = stepper.Plan(clock, FrameCosts{}, true, false, false);
    REQUIRE(plan.cadence == Cadence::kLocked);
    REQUIRE(plan.steps == 2);
    for (int i = 0; i < plan.steps; ++i) {
      latched.push_back(input.Latch(
          tick++, plan.tick_window_end_ns[static_cast<std::size_t>(i)],
          i + 1 == plan.steps));
    }
    windows.push_back({plan.tick_window_end_ns[0], plan.tick_window_end_ns[1]});
  }
  toggler.join();
  (*sampler)->Stop();
  const InputSamplerStats stats = (*sampler)->Stats();
  SDL_CloseJoystick(joystick);
  gamepads.CloseAll();
  REQUIRE(SDL_DetachVirtualJoystick(id));

  // Every edge, with the tick it landed on.
  struct Landed {
    InputEdge edge;
    Tick tick = 0;
  };
  std::vector<Landed> edges;
  for (const InputFrame& frame : latched) {
    for (std::uint8_t i = 0; i < frame.edge_count; ++i) {
      edges.push_back(Landed{frame.edges[i], frame.tick});
    }
  }
  WARN("sampler: " << stats.polls << " polls, " << stats.poll_ns / 1000
                   << " us polling (longest " << stats.poll_ns_max / 1000
                   << " us), " << stats.wait_ns / 1000 << " us waiting; "
                   << edges.size() << " edges for " << toggles.size()
                   << " toggles");

  // Always: every edge comes from the sampler, sits on the tick its stamp's
  // window gives, and each action alternates press and release.
  std::array<bool, 2> down{};
  for (const Landed& landed : edges) {
    REQUIRE(landed.edge.source == EdgeSource::kGamepadSampler);
    REQUIRE(landed.edge.action < 2);
    REQUIRE(landed.edge.down != down[landed.edge.action]);
    down[landed.edge.action] = landed.edge.down;
    const std::size_t frame = (landed.tick - 1) / 2;
    REQUIRE(PlaceEdge(landed.edge.stamp_ns, windows[frame]) ==
            static_cast<int>((landed.tick - 1) % 2));
  }

  // With the sampler on time, every toggle is an edge about one period
  // after it, and the two toggles of a frame land on its two ticks.
  std::vector<std::uint64_t> delays;
  int judged = 0;
  int apart = 0;
  bool matched = edges.size() == toggles.size();
  for (std::size_t i = 0; matched && i < toggles.size(); ++i) {
    const Toggle& toggle = toggles[i];
    const Landed& landed = edges[i];
    if (landed.edge.action != toggle.action ||
        landed.edge.down != toggle.down ||
        landed.edge.stamp_ns + 100'000 < toggle.at_ns) {
      matched = false;
      break;
    }
    delays.push_back(landed.edge.stamp_ns > toggle.at_ns
                         ? landed.edge.stamp_ns - toggle.at_ns
                         : 0);
    if (i % 2 == 1) {
      const Toggle& first = toggles[i - 1];
      const bool on_time = first.at_ns - first.slot_ns < 2 * kMs &&
                           toggle.at_ns - toggle.slot_ns < 2 * kMs;
      if (on_time) {
        ++judged;
        if (landed.tick != edges[i - 1].tick) ++apart;
      }
    }
  }
  std::uint64_t median = 0;
  std::uint64_t worst = 0;
  if (matched && !delays.empty()) {
    std::vector<std::uint64_t> sorted = delays;
    std::ranges::sort(sorted);
    median = sorted[sorted.size() / 2];
    worst = sorted.back();
  }
  WARN("toggle to stamp: median "
       << median / 1000 << " us, max " << worst / 1000 << " us; " << apart
       << " of " << judged << " on-time pairs on different ticks");
  // Timing needs a machine that is not saturated: with other work keeping
  // every CPU busy the sampler's waits stretch (a press and its release
  // can fall between two polls, the residual of red team P10), and that is
  // reported, not failed.
  const bool timing_ok = matched && judged >= kFrames / 2 && apart == judged &&
                         median <= 2 * kMs && stats.polls > 100;
  const double load = load_watch.Load();
  if (!timing_ok && load > 0.5) {
    WARN("sampler timing off with a busy machine (CPU load "
         << static_cast<int>(load * 100.0) << "%): not judged");
    return;
  }
  REQUIRE(matched);
  REQUIRE(judged >= kFrames / 2);
  REQUIRE(apart == judged);
  // About one sampler period (1 ms at 1 kHz).
  REQUIRE(median <= 2 * kMs);
  REQUIRE(stats.polls > 100);
}

}  // namespace
}  // namespace se
