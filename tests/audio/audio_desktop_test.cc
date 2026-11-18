// Copyright 2026 The simple-engine-2 Authors.
//
// The one test that opens the real playback device (label desktop): at
// master and group volume 0, so nothing is heard on this machine's
// speakers, for 1 s, then it closes cleanly. A machine without a device
// skips with the fallback's reason.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "se_audio/audio_engine.h"
#include "se_audio/pcm.h"
#include "support/test_roles.h"

namespace se::audio {
namespace {

TEST_CASE("The playback device opens at volume 0 for 1 s and closes",
          "[desktop][audio]") {
  const testing::MainThreadScope main;
  EngineConfig config;
  config.output = Output::kDevice;
  config.master_volume = 0.0F;
  config.group_volume = {0.0F, 0.0F};
  auto created = AudioEngine::Create(config);
  REQUIRE(created);
  std::unique_ptr<AudioEngine> engine = std::move(*created);
  if (engine->FellBack()) {
    SKIP("no playback device: " + engine->FallbackReason());
  }
  REQUIRE(engine->GetBackend() == Backend::kDevice);
  const std::uint32_t rate = engine->SampleRate();
  const std::uint32_t channels = engine->Channels();
  // Two seconds of silence, played at volume 0 as well.
  engine->SetSound(
      1, MakePcm(std::vector<float>(std::size_t{2} * rate * channels, 0.0F),
                 channels, rate));
  auto voice = engine->Play({.sound = 1, .volume = 0.0F});
  REQUIRE(voice);
  const std::uint64_t start_frames = engine->TimeFrames();
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(1)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    engine->Sweep();
  }
  const std::uint64_t mixed = engine->TimeFrames() - start_frames;
  INFO("the device mixed " << mixed << " frames in 1 s at " << rate << " Hz");
  // The device's thread mixed about a second (its periods are 10 ms).
  REQUIRE(mixed >= rate * 8 / 10);
  REQUIRE(mixed <= rate * 13 / 10);
  REQUIRE(engine->Playing(*voice));
  engine->StopAll();
  engine.reset();
}

}  // namespace
}  // namespace se::audio
