// Copyright 2026 The simple-engine-2 Authors.
//
// The sound-loading entry point (docs/milestone-specs.md M9 "Sounds"): the
// background lane reads a sound file and decodes it into PcmPayload in
// bounded steps; ma_engine stays on main. miniaudio is confined to M8's
// se_audio, so se_assets does not link it: se_audio implements
// SoundDecoder with miniaudio's decoder (data only, safe off main) and the
// integration hands it to the AssetRegistry (SetSoundDecoder) (ADR-081).
//
// The decoder must be safe to call from the lane thread while main runs;
// each Open() returns an independent stream.

#ifndef SE_ASSETS_SOUND_DECODER_H_
#define SE_ASSETS_SOUND_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "se_assets/background_lane.h"
#include "se_assets/payloads.h"
#include "se_core/error.h"

namespace se {

// One sound being decoded. Lane thread only.
class SoundDecodeStream {
 public:
  virtual ~SoundDecodeStream() = default;
  // Appends at most about 2 ms of decoding work to |pcm| (its channels and
  // sample rate are set by the first call). Returns true when the sound is
  // complete.
  [[nodiscard]] virtual std::expected<bool, Error> DecodeStep(
      PcmPayload& pcm) = 0;
};

class SoundDecoder {
 public:
  virtual ~SoundDecoder() = default;
  // Starts decoding |encoded| (the whole file: WAV or FLAC for M8).
  [[nodiscard]] virtual std::expected<std::unique_ptr<SoundDecodeStream>, Error>
  Open(std::vector<std::byte> encoded, std::string_view path) = 0;
};

// Payload: PcmPayload. Reads |path| in 1 MB steps, then decodes through
// |decoder| (which must outlive the job; a null decoder is an Error).
[[nodiscard]] std::unique_ptr<LaneJob> MakeSoundJob(std::string path,
                                                    SoundDecoder* decoder);

}  // namespace se

#endif  // SE_ASSETS_SOUND_DECODER_H_
