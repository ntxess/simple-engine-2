// Copyright 2026 The simple-engine-2 Authors.

#include "se_audio/pcm.h"

#include <miniaudio.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"

namespace se::audio {

std::expected<PcmRef, Error> DecodeSound(std::span<const std::byte> encoded,
                                         std::uint32_t sample_rate,
                                         std::uint32_t channels,
                                         std::string_view name) {
  const auto fail = [name](std::string message) {
    return std::unexpected(
        Error{ErrorCode::kParse, std::move(message), std::string(name), 0, 0});
  };
  if (encoded.empty()) return fail("audio: the sound file is empty");
  if (sample_rate == 0 || channels == 0) {
    return fail("audio: no sample rate or channel count to decode to");
  }
  ma_decoder_config config =
      ma_decoder_config_init(ma_format_f32, channels, sample_rate);
  ma_uint64 frames = 0;
  void* decoded = nullptr;
  const ma_result result = ma_decode_memory(encoded.data(), encoded.size(),
                                            &config, &frames, &decoded);
  if (result != MA_SUCCESS) {
    return fail(std::format("audio: cannot decode the sound: {}",
                            ma_result_description(result)));
  }
  auto pcm = std::make_shared<Pcm>();
  pcm->channels = channels;
  pcm->sample_rate = sample_rate;
  pcm->frames = frames;
  const auto* first = static_cast<const float*>(decoded);
  pcm->samples.assign(first,
                      first + static_cast<std::size_t>(frames) * channels);
  ma_free(decoded, nullptr);
  if (frames == 0) return fail("audio: the sound has no frames");
  return PcmRef(std::move(pcm));
}

PcmRef MakePcm(std::vector<float> samples, std::uint32_t channels,
               std::uint32_t sample_rate) {
  auto pcm = std::make_shared<Pcm>();
  pcm->channels = std::max<std::uint32_t>(channels, 1);
  pcm->sample_rate = sample_rate;
  pcm->frames = samples.size() / pcm->channels;
  pcm->samples = std::move(samples);
  return pcm;
}

}  // namespace se::audio
