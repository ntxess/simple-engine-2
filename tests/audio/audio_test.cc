// Copyright 2026 The simple-engine-2 Authors.
//
// se_audio and the AudioBridge (docs/milestone-specs.md M8 "Audio", Tests):
// everything here mixes in miniaudio's no-device mode (frames pulled with
// ReadFrames) or on its null backend, so nothing is ever heard. Label unit,
// everywhere; ASan and TSan in WSL. The one real-device test is in
// audio_desktop_test.cc.
//
//   - sounds from one frame's two ticks start 8.33 ms apart (+-1 frame at
//     48 kHz), by onset detection on the mixed output;
//   - the late and cosmetic rules; voice-cap stealing;
//   - the end ring and the sweeper;
//   - stop, scope release and reload while a mixer thread plays;
//   - the null-backend fallback;
//   - decoding WAV bytes; the PCM clock.

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "se_audio/audio_engine.h"
#include "se_audio/pcm.h"
#include "se_audio/pcm_clock.h"
#include "se_core/component_registry.h"
#include "se_core/core_components.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_runtime/audio_bridge.h"
#include "se_sim/audio_events.h"
#include "support/test_roles.h"

namespace se::audio {
namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kHit = SoundKeyOf("hit");
constexpr std::uint32_t kLong = SoundKeyOf("long");

// A burst: |frames| frames of a full-scale square wave, then nothing.
PcmRef Burst(std::uint32_t frames) {
  std::vector<float> samples(static_cast<std::size_t>(frames) * kChannels);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const float value = (i / 8) % 2 == 0 ? 0.9F : -0.9F;
    samples[std::size_t{i} * kChannels] = value;
    samples[std::size_t{i} * kChannels + 1] = value;
  }
  return MakePcm(std::move(samples), kChannels, kRate);
}

std::unique_ptr<AudioEngine> NoDeviceEngine(EngineConfig config = {})
    SE_REQUIRES(g_main) {
  config.output = Output::kNoDevice;
  config.sample_rate = kRate;
  config.channels = kChannels;
  auto engine = AudioEngine::Create(config);
  REQUIRE(engine);
  REQUIRE((*engine)->GetBackend() == Backend::kNoDevice);
  return std::move(*engine);
}

// Mixes |frames| frames in device-sized chunks of 480 (10 ms).
std::vector<float> Mix(AudioEngine& engine, std::uint32_t frames) {
  std::vector<float> out(static_cast<std::size_t>(frames) * kChannels);
  std::uint32_t done = 0;
  while (done < frames) {
    const std::uint32_t chunk = std::min<std::uint32_t>(480, frames - done);
    const std::span<float> part(out.data() + std::size_t{done} * kChannels,
                                static_cast<std::size_t>(chunk) * kChannels);
    REQUIRE(engine.ReadFrames(part) == chunk);
    done += chunk;
  }
  return out;
}

// Frames where the left channel turns loud after at least |quiet| silent
// frames.
std::vector<std::uint32_t> Onsets(const std::vector<float>& mixed,
                                  std::uint32_t quiet) {
  std::vector<std::uint32_t> onsets;
  std::uint32_t silent = quiet;
  for (std::size_t i = 0; i < mixed.size() / kChannels; ++i) {
    const float left = mixed[i * kChannels];
    if (std::fabs(left) > 0.3F) {
      if (silent >= quiet) onsets.push_back(static_cast<std::uint32_t>(i));
      silent = 0;
    } else {
      ++silent;
    }
  }
  return onsets;
}

TEST_CASE("Two ticks' sounds start 8.33 ms apart in the mixed output",
          "[audio]") {
  const testing::MainThreadScope main;
  auto engine = NoDeviceEngine();
  engine->SetSound(kHit, Burst(48));  // 1 ms

  // A World with audio events, drained after each of a 60 Hz Locked
  // frame's two ticks by the bridge, with D for 60 Hz Paced (B 5.5 ms).
  ComponentRegistry registry;
  RegisterCoreComponents(registry);
  registry.Seal();
  const InlineOwnerScope owner(OnMainThread());
  World world(registry, {.name = "audio"});
  AudioEvents& events = AddAudioEvents(world);

  const std::uint64_t frame_start = 2'000'000'000;
  std::uint64_t now = frame_start;
  AudioBridge bridge(engine.get(), [&now] { return now; });
  const std::uint64_t d = AudioDelayNs(5.5e-3, 1.0 / 60.0);
  REQUIRE(d == 13'833'333);
  bridge.BeginFrame(now, d);
  const std::uint64_t prev_start = frame_start - 16'666'667;
  for (const std::uint64_t window_end :
       {prev_start + 8'333'333, prev_start + 16'666'667}) {
    events.Push({.sound = kHit});
    bridge.Drain(world, window_end);
    REQUIRE(events.Empty());
  }
  REQUIRE(bridge.Stats().played == 2);
  REQUIRE(bridge.Stats().late == 0);

  const std::vector<float> mixed = Mix(*engine, 4800);
  const std::vector<std::uint32_t> onsets = Onsets(mixed, 48);
  REQUIRE(onsets.size() == 2);
  // 8.33 ms at 48 kHz is 400 frames.
  const std::int64_t apart = static_cast<std::int64_t>(onsets[1]) -
                             static_cast<std::int64_t>(onsets[0]);
  INFO("onsets at frames " << onsets[0] << " and " << onsets[1]);
  REQUIRE(std::abs(apart - 400) <= 1);
  // The first starts D after its anchor (frame 0 is now).
  const std::int64_t first = static_cast<std::int64_t>(onsets[0]);
  REQUIRE(std::abs(first - (13'833'333 - 8'333'333) * 48 / 1'000'000) <= 1);
}

TEST_CASE("Late gameplay sounds play at once; late cosmetic ones drop",
          "[audio]") {
  PcmClock clock(kRate);
  clock.Calibrate(1'000'000'000, 96'000);
  // On time: at the mapped frame.
  AudioStart start =
      ScheduleAudio(1'000'000'000, 10'000'000, 1'000'000'000, false, clock);
  REQUIRE(start.play);
  REQUIRE_FALSE(start.late);
  REQUIRE(start.start_frame == 96'000 + 480);
  // 50 ms late: both kinds play at once.
  for (const bool cosmetic : {false, true}) {
    start =
        ScheduleAudio(900'000'000, 50'000'000, 1'000'000'000, cosmetic, clock);
    REQUIRE(start.play);
    REQUIRE(start.late);
    REQUIRE(start.lateness_ns == 50'000'000);
    REQUIRE(start.start_frame == 0);
  }
  // 150 ms late: the cosmetic one is dropped, the gameplay one plays.
  start = ScheduleAudio(800'000'000, 50'000'000, 1'000'000'000, true, clock);
  REQUIRE_FALSE(start.play);
  start = ScheduleAudio(800'000'000, 50'000'000, 1'000'000'000, false, clock);
  REQUIRE(start.play);
  REQUIRE(start.start_frame == 0);
  // An anchor ahead of the platform clock starts no later than now + D.
  start = ScheduleAudio(5'000'000'000, 10'000'000, 1'000'000'000, false, clock);
  REQUIRE(start.start_frame == 96'000 + 480);
}

TEST_CASE("The bridge drops late cosmetic events and counts them", "[audio]") {
  const testing::MainThreadScope main;
  auto engine = NoDeviceEngine();
  engine->SetSound(kHit, Burst(48));
  ComponentRegistry registry;
  RegisterCoreComponents(registry);
  registry.Seal();
  const InlineOwnerScope owner(OnMainThread());
  World world(registry, {.name = "late"});
  AudioEvents& events = AddAudioEvents(world);
  std::uint64_t now = 3'000'000'000;
  AudioBridge bridge(engine.get(), [&now] { return now; });
  bridge.BeginFrame(now, 10'000'000);
  events.Push({.sound = kHit, .cosmetic = true});
  events.Push({.sound = kHit, .cosmetic = false});
  events.Push({.sound = SoundKeyOf("missing")});
  // The tick's anchor is 200 ms in the past.
  bridge.Drain(world, now - 200'000'000);
  REQUIRE(bridge.Stats().cosmetic_dropped == 1);
  REQUIRE(bridge.Stats().played == 1);
  REQUIRE(bridge.Stats().late == 1);
  REQUIRE(bridge.Stats().failed == 1);
  REQUIRE(engine->Stats().unknown_sounds == 1);
  REQUIRE(engine->ActiveVoices(Group::kEffects) == 1);
}

TEST_CASE("A group at its voice cap steals its oldest voice", "[audio]") {
  const testing::MainThreadScope main;
  EngineConfig config;
  config.voice_cap = {2, 3};
  auto engine = NoDeviceEngine(config);
  engine->SetSound(kLong, Burst(48'000));
  std::vector<VoiceId> effects;
  for (int i = 0; i < 4; ++i) {
    auto voice = engine->Play({.sound = kLong, .group = Group::kEffects});
    REQUIRE(voice);
    effects.push_back(*voice);
  }
  REQUIRE(engine->ActiveVoices(Group::kEffects) == 3);
  REQUIRE_FALSE(engine->Playing(effects[0]));
  REQUIRE(engine->Playing(effects[1]));
  REQUIRE(engine->Playing(effects[3]));
  REQUIRE(engine->Stats().voices_stolen == 1);
  // The other group is not touched.
  REQUIRE(engine->Play({.sound = kLong, .group = Group::kMusic}));
  REQUIRE(engine->Play({.sound = kLong, .group = Group::kMusic}));
  REQUIRE(engine->Play({.sound = kLong, .group = Group::kMusic}));
  REQUIRE(engine->ActiveVoices(Group::kMusic) == 2);
  REQUIRE(engine->ActiveVoices(Group::kEffects) == 3);
  REQUIRE(engine->Stats().voices_stolen == 2);
  // Caps that do not fit the voice table are an error.
  EngineConfig too_many;
  too_many.output = Output::kNoDevice;
  too_many.voice_cap = {200, 100};
  REQUIRE_FALSE(AudioEngine::Create(too_many));
}

TEST_CASE("Finished voices leave through the end ring or the sweep",
          "[audio]") {
  const testing::MainThreadScope main;
  auto engine = NoDeviceEngine();
  engine->SetSound(kHit, Burst(100));
  std::weak_ptr<const Pcm> pcm = engine->Sound(kHit);

  SECTION("the end callback's ring") {
    auto voice = engine->Play({.sound = kHit});
    REQUIRE(voice);
    // The sound reaches its end in the first chunk; miniaudio calls the
    // end callback in the next processing step.
    static_cast<void>(Mix(*engine, 960));
    engine->Sweep();
    REQUIRE_FALSE(engine->Playing(*voice));
    REQUIRE(engine->Stats().ended_by_ring == 1);
    REQUIRE(engine->Stats().ended_by_sweep == 0);
  }
  SECTION("the sweep, before the callback came") {
    auto voice = engine->Play({.sound = kHit});
    REQUIRE(voice);
    // Exactly to the end: at_end is set, the callback has not run.
    static_cast<void>(Mix(*engine, 100));
    static_cast<void>(Mix(*engine, 1));
    engine->Sweep();
    REQUIRE_FALSE(engine->Playing(*voice));
    REQUIRE(engine->Stats().ended_by_ring + engine->Stats().ended_by_sweep ==
            1);
  }
  REQUIRE(engine->Stats().voices_active == 0);
  // The sound table still holds the PCM; no voice does.
  REQUIRE(pcm.use_count() == 1);
}

TEST_CASE("Reloading a sound gives new voices the new PCM only", "[audio]") {
  const testing::MainThreadScope main;
  auto engine = NoDeviceEngine();
  engine->SetSound(kLong, Burst(4800));
  std::weak_ptr<const Pcm> old_pcm = engine->Sound(kLong);
  auto old_voice = engine->Play({.sound = kLong});
  REQUIRE(old_voice);
  engine->SetSound(kLong, Burst(9600));
  // The playing voice keeps the old PCM alive.
  REQUIRE_FALSE(old_pcm.expired());
  auto new_voice = engine->Play({.sound = kLong});
  REQUIRE(new_voice);
  REQUIRE(engine->Sound(kLong)->frames == 9600);
  static_cast<void>(Mix(*engine, 6000));
  engine->Sweep();
  // The old voice ended and let go of the old PCM; the new one plays on.
  REQUIRE_FALSE(engine->Playing(*old_voice));
  REQUIRE(old_pcm.expired());
  REQUIRE(engine->Playing(*new_voice));
}

TEST_CASE("Stop, scope release and reload are clean while a mixer plays",
          "[audio]") {
  const testing::MainThreadScope main;
  auto engine = NoDeviceEngine();
  engine->SetSound(kHit, Burst(240));
  engine->SetSound(kLong, Burst(24'000));
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> mixed{0};
  // The mixer, as miniaudio's device thread would be.
  std::vector<float> chunk(std::size_t{480} * kChannels);
  std::thread mixer([&engine, &stop, &mixed, &chunk] {
    while (!stop.load(std::memory_order_acquire)) {
      mixed.fetch_add(engine->ReadFrames(chunk), std::memory_order_relaxed);
    }
  });
  // The rounds below take about a millisecond: on a loaded machine the
  // mixer may not have run yet, so wait until it mixes (bounded).
  for (int i = 0; i < 100'000 && mixed.load(std::memory_order_relaxed) == 0;
       ++i) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  REQUIRE(mixed.load(std::memory_order_relaxed) > 0);
  std::uint32_t stopped = 0;
  for (int round = 0; round < 300; ++round) {
    const ScopeId scope = static_cast<ScopeId>(1 + round % 3);
    for (int i = 0; i < 6; ++i) {
      auto voice = engine->Play({.sound = i % 2 == 0 ? kHit : kLong,
                                 .group = Group::kEffects,
                                 .scope = scope,
                                 .start_frame = engine->TimeFrames() + 64});
      REQUIRE(voice);
      if (i == 5 && engine->Stop(*voice)) ++stopped;
    }
    if (round % 7 == 0) engine->SetSound(kLong, Burst(24'000));
    if (round % 5 == 0) engine->ReleaseScope(1 + (round / 5) % 3);
    engine->Sweep();
  }
  stop.store(true, std::memory_order_release);
  mixer.join();
  REQUIRE(mixed.load() > 0);
  REQUIRE(stopped > 0);
  engine->StopAll();
  REQUIRE(engine->Stats().voices_active == 0);
  // Destroying the engine with nothing left is the shutdown step.
  engine.reset();
}

TEST_CASE("Destroying a World ends its voices", "[audio]") {
  const testing::MainThreadScope main;
  auto engine = NoDeviceEngine();
  engine->SetSound(kLong, Burst(48'000));
  ComponentRegistry registry;
  RegisterCoreComponents(registry);
  registry.Seal();
  const InlineOwnerScope owner(OnMainThread());
  std::uint64_t now = 1'000'000'000;
  AudioBridge bridge(engine.get(), [&now] { return now; });
  bridge.BeginFrame(now, 10'000'000);
  auto keep = std::make_unique<World>(registry, WorldDesc{.name = "keep"});
  auto drop = std::make_unique<World>(registry, WorldDesc{.name = "drop"});
  AddAudioEvents(*keep).Push({.sound = kLong});
  AddAudioEvents(*drop).Push({.sound = kLong});
  AddAudioEvents(*drop).Push({.sound = kLong});
  bridge.Drain(*keep, now);
  bridge.Drain(*drop, now);
  REQUIRE(bridge.Stats().scopes == 2);
  REQUIRE(engine->ActiveVoices(Group::kEffects) == 3);
  drop.reset();
  REQUIRE(engine->ActiveVoices(Group::kEffects) == 1);
  keep.reset();
  REQUIRE(engine->ActiveVoices(Group::kEffects) == 0);
}

TEST_CASE("Without an engine the bridge drops events", "[audio]") {
  const testing::MainThreadScope main;
  ComponentRegistry registry;
  RegisterCoreComponents(registry);
  registry.Seal();
  const InlineOwnerScope owner(OnMainThread());
  World world(registry, {.name = "silent"});
  AudioEvents& events = AddAudioEvents(world);
  AudioBridge bridge(nullptr, [] { return std::uint64_t{5}; });
  bridge.BeginFrame(5, 1);
  events.Push({.sound = kHit});
  bridge.Drain(world, 5);
  REQUIRE(events.Empty());
  REQUIRE(bridge.Stats().no_engine == 1);
  bridge.Sweep();
  // More than a tick holds are dropped and counted.
  for (std::size_t i = 0; i < AudioEvents::kCapacity + 3; ++i) {
    events.Push({.sound = kHit});
  }
  REQUIRE(events.Events().size() == AudioEvents::kCapacity);
  REQUIRE(events.Dropped() == 3);
}

TEST_CASE("Without a device the engine runs on the null backend", "[audio]") {
  const testing::MainThreadScope main;
  EngineConfig config;
  config.output = Output::kDevice;
  config.fail_device_for_test = true;
  config.master_volume = 0.0F;
  auto engine = AudioEngine::Create(config);
  REQUIRE(engine);
  REQUIRE((*engine)->GetBackend() == Backend::kNull);
  REQUIRE((*engine)->FellBack());
  REQUIRE_FALSE((*engine)->FallbackReason().empty());
  (*engine)->SetSound(kHit, Burst(480));
  auto voice = (*engine)->Play({.sound = kHit});
  REQUIRE(voice);
  // The null backend's thread mixes in real time: the clock advances and
  // the voice ends.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((*engine)->Playing(*voice) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    (*engine)->Sweep();
  }
  REQUIRE((*engine)->TimeFrames() > 0);
  REQUIRE_FALSE((*engine)->Playing(*voice));

  config.output = Output::kNull;
  config.fail_device_for_test = false;
  auto null_engine = AudioEngine::Create(config);
  REQUIRE(null_engine);
  REQUIRE((*null_engine)->GetBackend() == Backend::kNull);
  REQUIRE_FALSE((*null_engine)->FellBack());
}

// A 16-bit PCM WAV file of |frames| mono frames at |rate|.
std::vector<std::byte> Wav(std::uint32_t rate, std::uint32_t frames) {
  std::vector<std::byte> out;
  const auto put = [&out](const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::byte*>(data);
    out.insert(out.end(), bytes, bytes + size);
  };
  const auto u32 = [&put](std::uint32_t v) { put(&v, 4); };
  const auto u16 = [&put](std::uint16_t v) { put(&v, 2); };
  put("RIFF", 4);
  u32(36 + frames * 2);
  put("WAVE", 4);
  put("fmt ", 4);
  u32(16);
  u16(1);
  u16(1);
  u32(rate);
  u32(rate * 2);
  u16(2);
  u16(16);
  put("data", 4);
  u32(frames * 2);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const auto sample = static_cast<std::int16_t>(i % 2 == 0 ? 16000 : -16000);
    put(&sample, 2);
  }
  return out;
}

TEST_CASE("WAV files decode to the engine's format", "[audio]") {
  auto same = DecodeSound(Wav(48000, 4800), 48000, 2, "same.wav");
  REQUIRE(same);
  REQUIRE((*same)->channels == 2);
  REQUIRE((*same)->frames == 4800);
  REQUIRE((*same)->samples.size() == 9600);
  REQUIRE(std::fabs((*same)->samples[0] - 16000.0F / 32768.0F) < 1e-3F);
  // Resampled from 24 kHz: twice the frames.
  auto resampled = DecodeSound(Wav(24000, 2400), 48000, 2, "half.wav");
  REQUIRE(resampled);
  REQUIRE((*resampled)->frames >= 4790);
  REQUIRE((*resampled)->frames <= 4810);
  const std::array<std::byte, 8> junk{};
  auto bad = DecodeSound(junk, 48000, 2, "junk.wav");
  REQUIRE_FALSE(bad);
  REQUIRE(bad.error().path == "junk.wav");
  REQUIRE_FALSE(DecodeSound({}, 48000, 2, "empty.wav"));
}

TEST_CASE("The PCM clock follows the staircase's leading edge", "[audio]") {
  PcmClock clock(kRate);
  REQUIRE_FALSE(clock.Calibrated());
  REQUIRE(clock.FrameAt(123) == 0);
  // A device mixing 480 frames every 10 ms; read at random points within.
  for (std::uint64_t ms = 0; ms < 200; ms += 3) {
    const std::uint64_t now = 1'000'000'000 + ms * 1'000'000;
    const std::uint64_t mixed = (ms / 10 + 1) * 480;
    clock.Calibrate(now, mixed);
  }
  // The mapping's spacing is exact.
  const std::uint64_t a = clock.FrameAt(1'500'000'000);
  const std::uint64_t b = clock.FrameAt(1'508'333'333);
  REQUIRE(b - a == 400);
  // It sits on the leading edge: the frame mixed right at a period's start.
  REQUIRE(clock.FrameAt(1'000'000'000) == 480);
  REQUIRE(clock.FramesFor(10'000'000) == 480);
  // A clock that goes back starts over.
  clock.Calibrate(2'000'000'000, 10);
  REQUIRE(clock.FrameAt(2'000'000'000) == 10);
}

}  // namespace
}  // namespace se::audio
