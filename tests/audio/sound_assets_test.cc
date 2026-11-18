// Copyright 2026 The simple-engine-2 Authors.
//
// The game's sounds through the background lane (docs/milestone-specs.md
// "Integration (M8 + M9 + M17a)"; ADR-087), headless: a real BackgroundLane
// and AssetRegistry with se_runtime's LaneSoundDecoder (se_audio's
// PcmDecoder underneath). Label unit, everywhere; TSan in WSL covers the
// lane thread decoding while main drains its completions. The App's own
// path, with the audio engine and a hot reload during playback, is
// tests/platform/sound_assets_desktop_test.cc.

#include "se_runtime/sound_assets.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "se_assets/asset_registry.h"
#include "se_assets/background_lane.h"
#include "se_audio/pcm.h"
#include "se_core/pcm.h"
#include "se_core/thread_role.h"
#include "se_platform/fs.h"
#include "se_sim/audio_events.h"
#include "support/test_roles.h"

#ifndef SE_TEST_OUT_DIR
#define SE_TEST_OUT_DIR "."
#endif

namespace se {
namespace {

void PutLe(std::vector<std::byte>& out, std::uint32_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) {
    out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFU));
  }
}

// A mono 16-bit WAV at 48 kHz: |seconds| of a sine at |hz|.
std::vector<std::byte> SineWav(double seconds, double hz) {
  constexpr std::uint32_t kRate = 48000;
  const auto frames = static_cast<std::uint32_t>(seconds * kRate);
  std::vector<std::byte> wav;
  const auto tag = [&wav](std::string_view text) {
    for (const char c : text) wav.push_back(static_cast<std::byte>(c));
  };
  tag("RIFF");
  PutLe(wav, 36 + frames * 2, 4);
  tag("WAVE");
  tag("fmt ");
  PutLe(wav, 16, 4);
  PutLe(wav, 1, 2);
  PutLe(wav, 1, 2);
  PutLe(wav, kRate, 4);
  PutLe(wav, kRate * 2, 4);
  PutLe(wav, 2, 2);
  PutLe(wav, 16, 2);
  tag("data");
  PutLe(wav, frames * 2, 4);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / kRate;
    const auto sample = static_cast<std::int16_t>(
        std::lround(16000.0 * std::sin(2.0 * std::numbers::pi * hz * t)));
    PutLe(wav, static_cast<std::uint16_t>(sample), 2);
  }
  return wav;
}

std::string Scratch(std::string_view name) {
  const std::string dir = fs::Join(SE_TEST_OUT_DIR, name);
  REQUIRE(fs::CreateDirectories(dir).has_value());
  return dir;
}

void NoWake() noexcept {}

struct Fixture {
  explicit Fixture(bool hot_reload, std::uint64_t scan_ns) : decoder(48000, 2) {
    LaneConfig config;
    config.wake_main = &NoWake;
    config.scan_period_ns = scan_ns;
    auto started = BackgroundLane::Start(config);
    REQUIRE(started.has_value());
    lane = std::move(*started);
    AssetRegistryConfig registry_config;
    registry_config.hot_reload = hot_reload;
    registry = std::make_unique<AssetRegistry>(*lane, nullptr, registry_config);
    registry->SetSoundDecoder(&decoder);
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  ~Fixture() {
    lane->Stop();
    registry->ReleaseAll();
  }

  // Frames until |done| or about 10 s.
  template <class F>
  bool Frames(F done) SE_REQUIRES(g_main) {
    for (int i = 0; i < 5000; ++i) {
      registry->BeginFrame();
      if (done()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  LaneSoundDecoder decoder;
  std::unique_ptr<BackgroundLane> lane;
  std::unique_ptr<AssetRegistry> registry;
};

TEST_CASE("Sounds decode on the background lane in bounded steps",
          "[sound_assets]") {
  const testing::MainThreadScope main;
  const std::string dir = Scratch("sound_assets_decode");
  const std::string path = fs::Join(dir, "tone.wav");
  REQUIRE(fs::WriteFile(path, SineWav(3.0, 440.0)).has_value());
  Fixture f(/*hot_reload=*/false, 0);
  const AssetScope scope = f.registry->CreateScope("sandbox");
  const Handle<SoundAsset> sound = f.registry->RequestSound(scope, path);
  REQUIRE(f.Frames([&] { return f.registry->ScopeResident(scope); }));
  REQUIRE(f.registry->State(sound) == AssetState::kResident);
  const std::shared_ptr<const Pcm> pcm = f.registry->Sound(sound);
  REQUIRE(pcm != nullptr);
  // Mono at 48 kHz, decoded to the engine's stereo.
  REQUIRE(pcm->channels == 2);
  REQUIRE(pcm->sample_rate == 48000);
  REQUIRE(pcm->frames == 144000);
  REQUIRE(pcm->samples.size() == 288000);
  // A quarter period in (27.27 frames at 440 Hz) is near the peak, in both
  // channels.
  constexpr std::size_t kQuarter = std::size_t{2} * 27;
  CHECK(pcm->samples[kQuarter] > 0.3F);
  CHECK(pcm->samples[kQuarter + 1] > 0.3F);

  const SoundDecodeStats stats = f.decoder.Stats();
  INFO("decode steps " << stats.steps << ", longest "
                       << stats.longest_step_ns / 1000 << " us");
  REQUIRE(stats.opened == 1);
  REQUIRE(stats.decoded == 1);
  // 144000 frames in chunks of 4096 within 1 ms steps: one step or more.
  REQUIRE(stats.steps >= 1);
  CHECK(stats.last_role == ThreadRole::kBackground);
}

TEST_CASE("A reloaded sound leaves the old PCM to whoever still holds it",
          "[sound_assets]") {
  const testing::MainThreadScope main;
  const std::string dir = Scratch("sound_assets_reload");
  const std::string path = fs::Join(dir, "tone.wav");
  REQUIRE(fs::WriteFile(path, SineWav(1.0, 440.0)).has_value());
  // The lane scans every 50 ms.
  Fixture f(/*hot_reload=*/true, 50'000'000);
  const AssetScope scope = f.registry->CreateScope("sandbox");
  const Handle<SoundAsset> sound = f.registry->RequestSound(scope, path);
  REQUIRE(f.Frames([&] { return f.registry->ScopeResident(scope); }));
  // A voice's reference to the first version.
  std::shared_ptr<const Pcm> playing = f.registry->Sound(sound);
  REQUIRE(playing != nullptr);
  const std::weak_ptr<const Pcm> old = playing;
  REQUIRE(f.registry->Version(sound) == 1);

  REQUIRE(fs::WriteFile(path, SineWav(0.5, 880.0)).has_value());
  REQUIRE(f.Frames([&] { return f.registry->Version(sound) >= 2; }));
  const std::shared_ptr<const Pcm> now = f.registry->Sound(sound);
  REQUIRE(now != nullptr);
  CHECK(now != playing);
  REQUIRE(now->frames == 24000);
  // The holder still reads the first version, intact.
  REQUIRE(playing->frames == 48000);
  CHECK_FALSE(old.expired());
  // The registry kept nothing of it: once the holder lets go, it is freed.
  playing.reset();
  CHECK(old.expired());
  REQUIRE(f.decoder.Stats().decoded >= 2);
}

TEST_CASE("An undecodable sound settles with its Error", "[sound_assets]") {
  const testing::MainThreadScope main;
  const std::string dir = Scratch("sound_assets_bad");
  const std::string path = fs::Join(dir, "noise.wav");
  const std::string text = "not a sound file";
  REQUIRE(fs::WriteFile(path, std::as_bytes(std::span(text))).has_value());
  Fixture f(/*hot_reload=*/false, 0);
  const AssetScope scope = f.registry->CreateScope("sandbox");
  const Handle<SoundAsset> sound = f.registry->RequestSound(scope, path);
  // A failed load settles: the scope counts as resident, the Error stays.
  REQUIRE(f.Frames([&] { return f.registry->ScopeResident(scope); }));
  CHECK(f.registry->State(sound) == AssetState::kFailed);
  const Error* error = f.registry->LastError(sound);
  REQUIRE(error != nullptr);
  CHECK(error->path == path);
  CHECK(f.registry->Sound(sound) == nullptr);
}

TEST_CASE("The sound table names sounds by their keys", "[sound_assets]") {
  auto table = ParseSoundTable(
      R"({"hit": "assets/sounds/hit.wav", "death": "assets/sounds/death.wav"})",
      "sound.json");
  REQUIRE(table.has_value());
  REQUIRE(table->size() == 2);
  REQUIRE((*table)[0].name == "hit");
  REQUIRE((*table)[0].file == "assets/sounds/hit.wav");
  CHECK((*table)[0].key == SoundKeyOf("hit"));
  CHECK((*table)[1].key == SoundKeyOf("death"));
  CHECK_FALSE(ParseSoundTable(R"({"hit": 3})", "sound.json").has_value());
  CHECK_FALSE(ParseSoundTable("[", "sound.json").has_value());
}

}  // namespace
}  // namespace se
