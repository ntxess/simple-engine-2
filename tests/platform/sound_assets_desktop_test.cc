// Copyright 2026 The simple-engine-2 Authors.
//
// The game's sounds through the background lane, in the real App with a
// window (docs/milestone-specs.md "Integration (M8 + M9 + M17a)"; label
// desktop, so ci.ps1 -Desktop runs it in msvc-dev and msvc-asan). Mixed on
// miniaudio's null backend: its thread mixes in real time, so the voices
// read their PCM while main swaps it, and nothing is heard.
//
// A sound is decoded on the lane (role kBackground) and handed to the
// engine before the World's first tick; the file then changes on disk
// while a voice plays it: the lane's scan re-decodes it, new voices get
// the new PCM, the playing voice keeps the old one, and the old PCM is
// freed only after that voice has ended (under ASan in msvc-asan).

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/support/test_app.h"
#include "se_audio/audio_engine.h"
#include "se_audio/pcm.h"
#include "se_core/thread_role.h"
#include "se_platform/fs.h"
#include "se_runtime/app.h"
#include "se_runtime/asset_runtime.h"
#include "se_runtime/audio_bridge.h"
#include "se_runtime/pacer_policy.h"
#include "se_runtime/sound_assets.h"
#include "se_sim/audio_events.h"
#include "support/test_roles.h"

#ifndef SE_TEST_OUT_DIR
#define SE_TEST_OUT_DIR "."
#endif

namespace se {
namespace {

using testing::RunTestApp;
using testing::TestGameSetup;

void PutLe(std::vector<std::byte>& out, std::uint32_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) {
    out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFU));
  }
}

// A mono 16-bit WAV file: |seconds| of a sine at |hz|.
std::vector<std::byte> SineWav(double seconds, double hz) {
  constexpr std::uint32_t kRate = 48000;
  const auto frames = static_cast<std::uint32_t>(seconds * kRate);
  std::vector<std::byte> wav;
  const auto put_tag = [&wav](std::string_view tag) {
    for (const char c : tag) wav.push_back(static_cast<std::byte>(c));
  };
  put_tag("RIFF");
  PutLe(wav, 36 + frames * 2, 4);
  put_tag("WAVE");
  put_tag("fmt ");
  PutLe(wav, 16, 4);
  PutLe(wav, 1, 2);  // PCM
  PutLe(wav, 1, 2);  // mono
  PutLe(wav, kRate, 4);
  PutLe(wav, kRate * 2, 4);
  PutLe(wav, 2, 2);
  PutLe(wav, 16, 2);
  put_tag("data");
  PutLe(wav, frames * 2, 4);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / kRate;
    const auto sample = static_cast<std::int16_t>(
        std::lround(8000.0 * std::sin(2.0 * std::numbers::pi * hz * t)));
    PutLe(wav, static_cast<std::uint16_t>(sample), 2);
  }
  return wav;
}

bool WriteBytes(const std::string& path, std::span<const std::byte> bytes) {
  return fs::WriteFile(path, bytes).has_value();
}

bool WriteText(const std::string& path, std::string_view text) {
  return WriteBytes(
      path, std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

TEST_CASE("A sound reloaded during playback frees its old PCM after its voice",
          "[desktop][sound_assets]") {
  const std::string root = fs::Join(SE_TEST_OUT_DIR, "sound_assets_reload");
  REQUIRE(fs::CreateDirectories(fs::Join(root, "sounds")).has_value());
  const std::string wav = fs::Join(root, "sounds/tone.wav");
  // The first version plays for 2.5 s, longer than the reload takes (the
  // lane scans every 500 ms); the second plays for 3 s, so its size differs
  // and a voice of it outlasts the first one's.
  REQUIRE(WriteBytes(wav, SineWav(2.5, 440.0)));
  REQUIRE(WriteText(fs::Join(root, "sounds.json"),
                    R"({"tone": "sounds/tone.wav"})"));
  const std::uint32_t key = SoundKeyOf("tone");

  struct Seen {
    int phase = 0;
    std::uint64_t first_tick_iterate = 0;
    bool resident_before_first_tick = false;
    std::weak_ptr<const Pcm> old_pcm;
    std::uint64_t old_frames = 0;
    std::uint64_t new_frames = 0;
    audio::VoiceId old_voice;
    audio::VoiceId new_voice;
    bool old_voice_kept_old_pcm = false;
    bool new_voice_got_new_pcm = false;
    bool old_alive_while_playing = false;
    bool old_freed_after_voice = false;
    bool new_voice_playing_then = false;
    SoundDecodeStats decode;
    std::uint64_t reload_wait_frames = 0;
    std::uint64_t start_ns = 0;
    bool timed_out = false;
  } seen;

  GameSetup setup = TestGameSetup();
  setup.sound_table = "sounds.json";
  setup.on_frame = [&](App& app, const FrameReport& report) {
    AssertMainThread();
    if (seen.start_ns == 0) seen.start_ns = report.now_ns;
    audio::AudioEngine* engine = app.Audio();
    REQUIRE(engine != nullptr);
    if (seen.first_tick_iterate == 0 && report.plan.steps > 0) {
      seen.first_tick_iterate = report.iterate;
      seen.resident_before_first_tick = engine->Sound(key) != nullptr;
    }
    switch (seen.phase) {
      case 0: {
        if (seen.first_tick_iterate == 0) break;
        const audio::PcmRef pcm = engine->Sound(key);
        REQUIRE(pcm != nullptr);
        seen.old_pcm = pcm;
        seen.old_frames = pcm->frames;
        auto voice = engine->Play({.sound = key});
        REQUIRE(voice);
        seen.old_voice = *voice;
        // The file changes while the voice plays it.
        REQUIRE(WriteBytes(wav, SineWav(3.0, 880.0)));
        seen.phase = 1;
        break;
      }
      case 1: {
        const audio::PcmRef now = engine->Sound(key);
        if (now == nullptr || now == seen.old_pcm.lock()) {
          ++seen.reload_wait_frames;
          break;
        }
        seen.new_frames = now->frames;
        seen.old_voice_kept_old_pcm =
            engine->Playing(seen.old_voice) &&
            engine->VoicePcm(seen.old_voice) == seen.old_pcm.lock();
        seen.old_alive_while_playing = !seen.old_pcm.expired();
        auto voice = engine->Play({.sound = key});
        REQUIRE(voice);
        seen.new_voice = *voice;
        seen.new_voice_got_new_pcm = engine->VoicePcm(seen.new_voice) == now;
        seen.phase = 2;
        break;
      }
      case 2:
        // The App sweeps at the end of every iterate: once the old voice
        // is gone, nothing holds the old PCM.
        if (engine->Playing(seen.old_voice)) break;
        seen.old_freed_after_voice = seen.old_pcm.expired();
        seen.new_voice_playing_then = engine->Playing(seen.new_voice);
        seen.decode = app.SoundDecodes();
        seen.phase = 3;
        app.RequestQuit();
        break;
      default:
        break;
    }
    if (report.now_ns - seen.start_ns > 20'000'000'000ULL && seen.phase < 3) {
      seen.timed_out = true;
      seen.phase = 3;
      app.RequestQuit();
    }
  };
  const int result = RunTestApp(
      std::move(setup),
      {"--content-root", root, "--audio", "null", "--hot-reload", "on"});
  REQUIRE(result == 0);
  REQUIRE_FALSE(seen.timed_out);
  INFO("waited " << seen.reload_wait_frames << " frames for the reload");
  CHECK(seen.resident_before_first_tick);
  REQUIRE(seen.old_frames == 120000);
  REQUIRE(seen.new_frames == 144000);
  CHECK(seen.old_voice_kept_old_pcm);
  CHECK(seen.old_alive_while_playing);
  CHECK(seen.new_voice_got_new_pcm);
  CHECK(seen.old_freed_after_voice);
  CHECK(seen.new_voice_playing_then);
  // Both versions were decoded on the background lane.
  REQUIRE(seen.decode.decoded >= 2);
  CHECK(seen.decode.last_role == ThreadRole::kBackground);
}

TEST_CASE("The audio delay D comes from the FramePacer's frame",
          "[desktop][sound_assets]") {
  struct Seen {
    int frames = 0;
    int matched = 0;
    int from_display = 0;
    double vblank_sec = 0.0;
    double budget_sec = 0.0;
    std::uint64_t delay_ns = 0;
  } seen;
  GameSetup setup = TestGameSetup();
  setup.on_frame = [&seen](App& app, const FrameReport& report) {
    AssertMainThread();
    // D = B + T/2 with B of this frame's plan and T the vblank period the
    // pacer's BeginFrame chose for this frame.
    if (report.iterate > 2 && report.pacer.vblank_period_sec > 0.0) {
      ++seen.frames;
      if (app.AudioDelay() ==
          AudioDelayNs(report.plan.budget_b, report.pacer.vblank_period_sec)) {
        ++seen.matched;
      }
      if (report.pacer.period_source != PeriodSource::kFallback) {
        ++seen.from_display;
      }
      seen.vblank_sec = report.pacer.vblank_period_sec;
      seen.budget_sec = report.plan.budget_b;
      seen.delay_ns = app.AudioDelay();
    }
    if (report.iterate >= 90) app.RequestQuit();
  };
  REQUIRE(RunTestApp(std::move(setup), {"--audio", "null"}) == 0);
  INFO("last frame: T " << seen.vblank_sec * 1e3 << " ms, B "
                        << seen.budget_sec * 1e3 << " ms, D "
                        << seen.delay_ns / 1e6 << " ms");
  REQUIRE(seen.frames > 0);
  CHECK(seen.matched == seen.frames);
  // The window's display gave the period (the vblank sampler or the OS's
  // mode), not the pacer's 60 Hz fallback.
  REQUIRE(seen.from_display > 0);
  REQUIRE(seen.delay_ns > 0);
}

}  // namespace
}  // namespace se
