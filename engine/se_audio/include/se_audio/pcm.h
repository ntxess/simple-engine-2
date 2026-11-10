// Copyright 2026 The simple-engine-2 Authors.
//
// Decoded sound data (docs/milestone-specs.md M8; ADR-076, ADR-087):
// interleaved 32-bit float PCM at the engine's rate and channel count, held
// by std::shared_ptr<const Pcm>. Pcm is se_core's type, the one the
// background lane decodes into and the AssetRegistry keeps, so a sound goes
// from the lane to the voices without a copy. The engine's sound table and
// every voice that plays it hold a reference; a voice drops its reference
// only after its ma_sound_uninit has returned, so the mixer never reads
// freed samples.
//
// DecodeSound and PcmDecoder use miniaudio's decoder (WAV and FLAC; MP3 is
// built into miniaudio too) and convert to the requested format. They
// touch no engine state, so any thread may call them: the App's sounds are
// decoded on the background lane through PcmDecoder, in steps
// (se_runtime/sound_assets.h).

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
#include "se_core/pcm.h"

namespace se::audio {

using Pcm = ::se::Pcm;
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

// One sound decoded in pieces, so that the caller bounds each piece (the
// background lane's steps of about 2 ms). It owns the encoded bytes. Any
// thread, one at a time.
class PcmDecoder {
 public:
  // Starts decoding |encoded| to |sample_rate| and |channels|. An empty or
  // unrecognised file is an Error naming |name|.
  [[nodiscard]] static std::expected<std::unique_ptr<PcmDecoder>, Error> Open(
      std::vector<std::byte> encoded, std::uint32_t sample_rate,
      std::uint32_t channels, std::string_view name);

  PcmDecoder(const PcmDecoder&) = delete;
  PcmDecoder& operator=(const PcmDecoder&) = delete;
  ~PcmDecoder();

  // Appends at most |max_frames| frames to |pcm| (whose format it sets).
  // Returns true when the sound has ended; a sound with no frames at all is
  // an Error.
  [[nodiscard]] std::expected<bool, Error> Decode(Pcm& pcm,
                                                  std::uint64_t max_frames);

  // The sound's length in frames when the format says (0: unknown).
  [[nodiscard]] std::uint64_t LengthFrames() const noexcept;

  struct Impl;

 private:
  explicit PcmDecoder(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace se::audio

#endif  // SE_AUDIO_PCM_H_
