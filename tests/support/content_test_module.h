// Copyright 2026 The simple-engine-2 Authors.
//
// The content test module (M13a): a game module whose states build their
// Worlds from texts read through StateEnv::content, for the engine's tests
// of the content gate, the runtime's start state, focus loss, empty stacks,
// content bundles and dumps of multi-state sessions. rampage's own states
// are M13b's; this module needs nothing from them.
//
//   title   game_start_state: the World of content/title.txt
//   play    start_state: the World of content/play.txt and the texts it
//           includes (content/common.txt)
//   pause   pushed over play: the World of content/pause.txt
//
// A text is lines of "entity NAME X Y VX VY" (an entity drifting by
// (VX, VY) per tick) and "include PATH" (another text whose entities join;
// what ContentTestDependencies reports). Every World holds a stamp of the
// hashes of the texts it was built from, so a changed text changes the
// hash of the next World built from it.
//
// The flow, one transition per tick at most, from the tick's InputFrame:
//   title: kStart pressed replaces it with play; kQuit pops it
//   play:  kPause pushes pause
//   pause: kPause pops it; kQuit pops it and replaces play with title

#ifndef TESTS_SUPPORT_CONTENT_TEST_MODULE_H_
#define TESTS_SUPPORT_CONTENT_TEST_MODULE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/component_registry.h"
#include "se_core/input_frame.h"
#include "se_core/types.h"
#include "se_sim/content_source.h"
#include "se_sim/game_module.h"

namespace se::testing {

// Moves its entity by |velocity| every tick.
struct Drift {
  Vec2 velocity;
};

// The texts a World was built from: their hashes, folded.
struct ContentStamp {
  std::uint64_t texts = 0;
};

// Registers Drift and ContentStamp (content_test_components.cc).
void RegisterContentTestComponents(ComponentRegistry& registry);

inline constexpr std::string_view kContentTitle = "title";
inline constexpr std::string_view kContentPlay = "play";
inline constexpr std::string_view kContentPause = "pause";

enum ContentAction : ActionId {
  kContentStart = 0,
  kContentPauseKey = 1,
  kContentQuit = 2,
};

[[nodiscard]] const GameModule& ContentTestModule();

// The text a state builds its World from (content-relative).
[[nodiscard]] std::string_view ContentStateText(std::string_view state_id);

// The "include" lines of |text|: what a text needs loaded with it.
[[nodiscard]] std::vector<std::string> ContentTestDependencies(
    std::string_view path, std::string_view text);

// tests/fixtures/ (the content root of the module's texts) and a source
// that reads it synchronously.
[[nodiscard]] std::string ContentFixtureRoot();
[[nodiscard]] CachedContentSource ContentFixtureSource();

// A frame for |tick| with |action| pressed (and nothing held).
[[nodiscard]] InputFrame PressFrame(Tick tick, ActionId action);

// An InputSource that presses actions at given ticks.
class ScriptedPresses final : public InputSource {
 public:
  struct Press {
    Tick tick = 0;
    ActionId action = 0;
  };
  explicit ScriptedPresses(std::vector<Press> presses)
      : presses_(std::move(presses)) {}
  InputFrame Latch(Tick tick, std::uint64_t tick_window_end_ns,
                   bool last_tick_of_frame) override;

 private:
  std::vector<Press> presses_;
};

}  // namespace se::testing

#endif  // TESTS_SUPPORT_CONTENT_TEST_MODULE_H_
