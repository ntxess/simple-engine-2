// Copyright 2026 The simple-engine-2 Authors.
//
// Decoded sound data (docs/milestone-specs.md M8; ADR-076): interleaved
// 32-bit float PCM at the engine's rate and channel count, held by
// std::shared_ptr<const Pcm>. The engine's sound table and every voice that
// plays it hold a reference; a voice drops its reference only after its
// ma_sound_uninit has returned, so the mixer never reads freed samples.
//
// DecodeSound uses miniaudio's decoder (WAV and FLAC; MP3 is built into
// miniaudio too) and converts to the requested format. It touches no engine
// state, so any thread may call it (M9's background lane will).

#ifndef SE_AUDIO_PCM_H_
#define SE_AUDIO_PCM_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "se_core/error.h"

namespace se::audio {

struct Pcm {
  // frames * channels samples, interleaved.
  std::vector<float> samples;
  std::uint32_t channels = 0;
  std::uint32_t sample_rate = 0;
  std::uint64_t frames = 0;
};

using PcmRef = std::shared_ptr<const Pcm>;

// Decodes |encoded| (a WAV or FLAC file's bytes) to PCM at |sample_rate|
// and |channels|. |name| names the sound in errors (a path). An empty or
// undecodable file is an Error.
[[nodiscard]] std::expected<PcmRef, Error> DecodeSound(
    std::span<const std::byte> encoded, std::uint32_t sample_rate,
    std::uint32_t channels, std::string_view name);

// PCM made from samples already in the engine's format (tests, tools).
[[nodiscard]] PcmRef MakePcm(std::vector<float> samples, std::uint32_t channels,
                             std::uint32_t sample_rate);

}  // namespace se::audio

#endif  // SE_AUDIO_PCM_H_
