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
namespace {

std::unexpected<Error> DecodeError(std::string_view name, std::string message) {
  return std::unexpected(
      Error{ErrorCode::kParse, std::move(message), std::string(name), 0, 0});
}

}  // namespace

std::expected<PcmRef, Error> DecodeSound(std::span<const std::byte> encoded,
                                         std::uint32_t sample_rate,
                                         std::uint32_t channels,
                                         std::string_view name) {
  if (encoded.empty()) {
    return DecodeError(name, "audio: the sound file is empty");
  }
  if (sample_rate == 0 || channels == 0) {
    return DecodeError(name,
                       "audio: no sample rate or channel count to decode to");
  }
  ma_decoder_config config =
      ma_decoder_config_init(ma_format_f32, channels, sample_rate);
  ma_uint64 frames = 0;
  void* decoded = nullptr;
  const ma_result result = ma_decode_memory(encoded.data(), encoded.size(),
                                            &config, &frames, &decoded);
  if (result != MA_SUCCESS) {
    return DecodeError(name, std::format("audio: cannot decode the sound: {}",
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
  if (frames == 0) return DecodeError(name, "audio: the sound has no frames");
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

struct PcmDecoder::Impl {
  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  ~Impl() {
    if (ready) ma_decoder_uninit(&decoder);
  }

  // The decoder reads these bytes in place: they live as long as it does.
  std::vector<std::byte> encoded;
  std::string name;
  std::uint32_t sample_rate = 0;
  std::uint32_t channels = 0;
  ma_decoder decoder{};
  bool ready = false;
  bool ended = false;
  std::uint64_t length = 0;
};

std::expected<std::unique_ptr<PcmDecoder>, Error> PcmDecoder::Open(
    std::vector<std::byte> encoded, std::uint32_t sample_rate,
    std::uint32_t channels, std::string_view name) {
  if (encoded.empty()) {
    return DecodeError(name, "audio: the sound file is empty");
  }
  if (sample_rate == 0 || channels == 0) {
    return DecodeError(name,
                       "audio: no sample rate or channel count to decode to");
  }
  auto impl = std::make_unique<Impl>();
  impl->encoded = std::move(encoded);
  impl->name = std::string(name);
  impl->sample_rate = sample_rate;
  impl->channels = channels;
  const ma_decoder_config config =
      ma_decoder_config_init(ma_format_f32, channels, sample_rate);
  if (const ma_result result = ma_decoder_init_memory(
          impl->encoded.data(), impl->encoded.size(), &config, &impl->decoder);
      result != MA_SUCCESS) {
    return DecodeError(name, std::format("audio: cannot decode the sound: {}",
                                         ma_result_description(result)));
  }
  impl->ready = true;
  ma_uint64 length = 0;
  if (ma_decoder_get_length_in_pcm_frames(&impl->decoder, &length) ==
      MA_SUCCESS) {
    impl->length = length;
  }
  return std::unique_ptr<PcmDecoder>(new PcmDecoder(std::move(impl)));
}

PcmDecoder::PcmDecoder(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PcmDecoder::~PcmDecoder() = default;

std::uint64_t PcmDecoder::LengthFrames() const noexcept {
  return impl_->length;
}

std::expected<bool, Error> PcmDecoder::Decode(Pcm& pcm,
                                              std::uint64_t max_frames) {
  Impl& impl = *impl_;
  pcm.channels = impl.channels;
  pcm.sample_rate = impl.sample_rate;
  if (impl.ended) return true;
  if (pcm.samples.empty() && impl.length > 0) {
    pcm.samples.reserve(static_cast<std::size_t>(impl.length) * impl.channels);
  }
  const std::size_t start = pcm.samples.size();
  pcm.samples.resize(start +
                     static_cast<std::size_t>(max_frames) * impl.channels);
  ma_uint64 read = 0;
  const ma_result result = ma_decoder_read_pcm_frames(
      &impl.decoder, pcm.samples.data() + start, max_frames, &read);
  pcm.samples.resize(start + static_cast<std::size_t>(read) * impl.channels);
  pcm.frames += read;
  if (result != MA_SUCCESS && result != MA_AT_END) {
    return DecodeError(impl.name, std::format("audio: decoding failed: {}",
                                              ma_result_description(result)));
  }
  impl.ended = result == MA_AT_END || read < max_frames;
  if (impl.ended && pcm.frames == 0) {
    return DecodeError(impl.name, "audio: the sound has no frames");
  }
  return impl.ended;
}

}  // namespace se::audio
