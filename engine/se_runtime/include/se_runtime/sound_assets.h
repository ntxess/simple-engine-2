// Copyright 2026 The simple-engine-2 Authors.
//
// The game's sounds through the background lane (docs/milestone-specs.md
// "Integration (M8 + M9 + M17a)"; ADR-087). M8's sounds are decoded on M9's
// lane, never on main:
//
//   boot step 9    LoadSoundTable: the sound table (config/sound.json, name
//                  to file) is read; its files are every state's default
//                  sounds (AssetRuntimeConfig::sounds)
//   boot step 10   the audio engine, then a LaneSoundDecoder at its rate
//                  and channel count is the AssetRegistry's SoundDecoder:
//                  each state's AssetScope requests its sounds (Urgent),
//                  the lane reads each file in 1 MB steps and decodes it
//                  here in steps of about 1 ms (miniaudio's decoder, data
//                  only; ma_engine stays on main)
//   iterate start  AssetRuntime's sound listener hands every sound that
//                  became resident, or was hot-reloaded, to the engine
//                  (AudioEngine::SetSound): new voices get the new PCM, the
//                  voices already playing keep the old one until they end
//
// The PCM is one object from the lane to the voices (se_core/pcm.h): the
// registry, the engine's table and each voice share it.

#ifndef SE_RUNTIME_SOUND_ASSETS_H_
#define SE_RUNTIME_SOUND_ASSETS_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "se_assets/sound_decoder.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"

namespace se {

// One row of the sound table.
struct SoundTableEntry {
  std::string name;
  // Relative to the content root, as written.
  std::string file;
  // SoundKeyOf(name): what AudioEvents and the engine name it by.
  std::uint32_t key = 0;
};

// Parses a sound table (an object of "name": "file" strings, in file
// order). A duplicate name, or two names with one key, is an Error.
[[nodiscard]] std::expected<std::vector<SoundTableEntry>, Error>
ParseSoundTable(std::string_view json, std::string_view source);

// Reads and parses |table| (relative to |content_root|).
[[nodiscard]] std::expected<std::vector<SoundTableEntry>, Error> LoadSoundTable(
    std::string_view content_root, std::string_view table);

struct SoundDecodeStats {
  // Sounds opened, decoded to the end, and decode steps run.
  std::uint64_t opened = 0;
  std::uint64_t decoded = 0;
  std::uint64_t steps = 0;
  // The longest decode step, in nanoseconds.
  std::uint64_t longest_step_ns = 0;
  // The role of the thread that ran the last step (kBackground on the
  // lane).
  ThreadRole last_role = ThreadRole::kLibrary;
};

// se_assets' SoundDecoder on se_audio's PcmDecoder. Open and DecodeStep
// run on the lane thread; Stats() from any thread. It must outlive every
// job that holds it (the App keeps it until after shutdown step 10).
class LaneSoundDecoder final : public SoundDecoder {
 public:
  // Frames decoded between two looks at the step's clock.
  static constexpr std::uint64_t kChunkFrames = 4096;
  // A step decodes chunks until this much time has passed.
  static constexpr std::uint64_t kStepBudgetNs = 1'000'000;

  // Decodes to |sample_rate| and |channels| (the engine's format).
  LaneSoundDecoder(std::uint32_t sample_rate, std::uint32_t channels) noexcept;

  [[nodiscard]] std::expected<std::unique_ptr<SoundDecodeStream>, Error> Open(
      std::vector<std::byte> encoded, std::string_view path) override;

  [[nodiscard]] std::uint32_t SampleRate() const noexcept {
    return sample_rate_;
  }
  [[nodiscard]] std::uint32_t Channels() const noexcept { return channels_; }
  [[nodiscard]] SoundDecodeStats Stats() const noexcept;

 private:
  class Stream;

  const std::uint32_t sample_rate_;
  const std::uint32_t channels_;
  std::atomic<std::uint64_t> opened_{0};
  std::atomic<std::uint64_t> decoded_{0};
  std::atomic<std::uint64_t> steps_{0};
  std::atomic<std::uint64_t> longest_step_ns_{0};
  std::atomic<ThreadRole> last_role_{ThreadRole::kLibrary};
};

}  // namespace se

#endif  // SE_RUNTIME_SOUND_ASSETS_H_
