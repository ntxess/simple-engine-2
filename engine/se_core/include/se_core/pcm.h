// Copyright 2026 The simple-engine-2 Authors.
//
// Decoded sound data: interleaved 32-bit float samples. One type for the
// whole path (ADR-087): the background lane decodes a sound into it
// (se_assets' PcmPayload), the AssetRegistry keeps it as
// std::shared_ptr<const Pcm>, and the audio engine's voices read that same
// object in place (se_audio's PcmRef). A hot reload replaces the
// registry's and the engine's pointers; voices already playing keep the old
// object alive until their ma_sound_uninit has returned.

#ifndef SE_CORE_PCM_H_
#define SE_CORE_PCM_H_

#include <cstdint>
#include <vector>

namespace se {

struct Pcm {
  // frames * channels samples, interleaved.
  std::vector<float> samples;
  std::uint32_t channels = 0;
  std::uint32_t sample_rate = 0;
  std::uint64_t frames = 0;
};

}  // namespace se

#endif  // SE_CORE_PCM_H_
