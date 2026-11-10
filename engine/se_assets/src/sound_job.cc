// Copyright 2026 The simple-engine-2 Authors.

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "se_assets/background_lane.h"
#include "se_assets/lane_jobs.h"
#include "se_assets/payloads.h"
#include "se_assets/sound_decoder.h"
#include "se_core/error.h"

namespace se {
namespace {

class SoundJob final : public LaneJob {
 public:
  SoundJob(std::string path, SoundDecoder* decoder)
      : read_(std::move(path)), decoder_(decoder) {}

  [[nodiscard]] std::string_view Kind() const noexcept override {
    return "sound";
  }

  StepResult Step(LaneContext& /*lane*/) override {
    if (decoder_ == nullptr) {
      return End(std::unexpected(
          Error{ErrorCode::kUnsupported,
                "no sound decoder: se_audio provides one (M8 integration)",
                read_.Path(), 0, 0}));
    }
    if (!read_.Done()) {
      auto done = read_.Step();
      if (!done) return End(std::unexpected(std::move(done.error())));
      return StepResult::Continue();
    }
    if (!stream_) {
      auto stream = decoder_->Open(std::move(read_.Bytes()), read_.Path());
      if (!stream) return End(std::unexpected(std::move(stream.error())));
      stream_ = std::move(*stream);
      return StepResult::Continue();
    }
    auto done = stream_->DecodeStep(pcm_);
    if (!done) {
      Error error = std::move(done.error());
      if (error.path.empty()) error.path = read_.Path();
      return End(std::unexpected(std::move(error)));
    }
    if (!*done) return StepResult::Continue();
    stream_.reset();
    return End(std::move(pcm_));
  }

  std::expected<LanePayload, Error> TakeResult() override {
    if (!result_) return std::unexpected(std::move(result_.error()));
    return LanePayload(std::move(*result_));
  }

 private:
  StepResult End(std::expected<PcmPayload, Error> result) {
    result_ = std::move(result);
    return StepResult::Done();
  }

  SteppedRead read_;
  SoundDecoder* decoder_;
  std::unique_ptr<SoundDecodeStream> stream_;
  PcmPayload pcm_;
  std::expected<PcmPayload, Error> result_;
};

}  // namespace

std::unique_ptr<LaneJob> MakeSoundJob(std::string path, SoundDecoder* decoder) {
  return std::make_unique<SoundJob>(std::move(path), decoder);
}

}  // namespace se
