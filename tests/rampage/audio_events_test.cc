// Copyright 2026 The simple-engine-2 Authors.
//
// rampage's M8 additions: the rules ask for sounds (EffectSystem's "hit"
// when HP is damaged, DeathRule's cosmetic "death") without changing the
// simulation, and IntentSystem keeps the discrete actions in the player's
// input buffer window.

#include "se_sim/audio_events.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "game_rampage/components.h"
#include "game_rampage/module.h"
#include "game_rampage/systems.h"
#include "rampage/support/rampage_session.h"
#include "rampage/support/test_world.h"
#include "se_core/input_buffer.h"
#include "se_core/input_frame.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/world.h"
#include "se_sim/sim_driver.h"
#include "support/test_roles.h"

namespace se::rampage {
namespace {

// Counts the sounds a session asks for, then drops them as the null sink.
class CountingSink final : public AudioSink {
 public:
  void Drain(World& world, std::uint64_t /*tick_window_end_ns*/)
      SE_REQUIRES(g_sim_owner) override {
    AudioEvents* events = FindAudioEvents(world);
    if (events == nullptr) return;
    for (const AudioEvent& event : events->Events()) {
      if (event.sound == kSoundHit) {
        ++hits;
        if (event.cosmetic || !event.positional) ++malformed;
      } else if (event.sound == kSoundDeath) {
        ++deaths;
        if (!event.cosmetic) ++malformed;
      } else {
        ++malformed;
      }
    }
    events->Clear();
  }
  std::size_t hits = 0;
  std::size_t deaths = 0;
  std::size_t malformed = 0;
};

TEST_CASE("The rules ask for sounds and the hashes do not change",
          "[rampage][audio]") {
  const testing::MainThreadScope main;
  testing::RampageSessionOptions options;
  options.scene = testing::ReferenceScenarioRequest("effects");
  options.ticks = 3600;
  const testing::RampageSessionResult silent =
      testing::RunRampageSession(options);
  REQUIRE(silent.error.empty());

  CountingSink sink;
  options.audio = &sink;
  const testing::RampageSessionResult counted =
      testing::RunRampageSession(options);
  REQUIRE(counted.error.empty());
  INFO(sink.hits << " hits, " << sink.deaths << " deaths");
  REQUIRE(sink.hits > 0);
  REQUIRE(sink.deaths > 0);
  REQUIRE(sink.malformed == 0);
  REQUIRE(counted.hashes == silent.hashes);
}

TEST_CASE("Discrete presses stay buffered for their window",
          "[rampage][input_buffer]") {
  const testing::MainThreadScope main;
  const InlineOwnerScope owner(OnMainThread());
  testing::RampageTestWorld world;
  const entt::entity player = world.Add({100.0F, 100.0F});
  world.Reg().emplace<PlayerControl>(player);
  world.Reg().emplace<MoveIntent>(player);
  world.Reg().emplace<KinematicBody>(player);
  world.Finish();

  // Movement alone leaves no buffer.
  world.Step(3, 1ULL << static_cast<int>(Action::kRight));
  REQUIRE(world.Reg().try_get<ActionBuffer>(player) == nullptr);

  InputFrame jump;
  jump.tick = world.Now() + 1;
  jump.pressed[static_cast<int>(Action::kJump)] = 1;
  world.StepFrame(jump);
  const Tick pressed_at = world.Now();
  const auto* buffer = world.Reg().try_get<ActionBuffer>(player);
  REQUIRE(buffer != nullptr);
  const auto action = static_cast<ActionId>(Action::kJump);
  REQUIRE(BufferWindow(buffer->buffer, action) == 8);
  REQUIRE(BufferWindow(buffer->buffer, static_cast<ActionId>(Action::kA1)) ==
          10);
  REQUIRE(BufferWindow(buffer->buffer, static_cast<ActionId>(Action::kExit)) ==
          6);
  // Held movement is not buffered.
  REQUIRE_FALSE(IsBuffered(buffer->buffer,
                           static_cast<ActionId>(Action::kRight), pressed_at));
  for (int i = 0; i < 7; ++i) {
    REQUIRE(IsBuffered(buffer->buffer, action, world.Now()));
    world.Step();
  }
  REQUIRE(IsBuffered(buffer->buffer, action, world.Now()));
  REQUIRE(world.Now() == pressed_at + 7);
  world.Step();
  REQUIRE_FALSE(IsBuffered(buffer->buffer, action, world.Now()));
  REQUIRE(buffer->buffer.pressed_at[action] == 0);
}

}  // namespace
}  // namespace se::rampage
