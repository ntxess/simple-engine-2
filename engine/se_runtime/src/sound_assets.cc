// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/sound_assets.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_assets/payloads.h"
#include "se_assets/sound_decoder.h"
#include "se_audio/pcm.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_platform/fs.h"
#include "se_runtime/user_settings.h"
#include "se_sim/audio_events.h"

namespace se {

std::expected<std::vector<SoundTableEntry>, Error> ParseSoundTable(
    std::string_view json, std::string_view source) {
  auto table = ParseNameTable(json, source);
  if (!table) return std::unexpected(std::move(table.error()));
  std::vector<SoundTableEntry> entries;
  entries.reserve(table->size());
  for (auto& [name, file] : *table) {
    const std::uint32_t key = SoundKeyOf(name);
    const auto same = std::ranges::find(entries, key, &SoundTableEntry::key);
    if (same != entries.end()) {
      return std::unexpected(Error{
          ErrorCode::kInvalidArgument,
          std::format("sound table: '{}' and '{}' have the same key {:#010x}",
                      same->name, name, key),
          std::string(source), 0, 0});
    }
    entries.push_back({std::move(name), std::move(file), key});
  }
  return entries;
}

std::expected<std::vector<SoundTableEntry>, Error> LoadSoundTable(
    std::string_view content_root, std::string_view table) {
  const std::string path = fs::Join(content_root, table);
  auto text = fs::ReadTextFile(path);
  if (!text) {
    Error error = std::move(text.error());
    if (error.path.empty()) error.path = path;
    return std::unexpected(std::move(error));
  }
  return ParseSoundTable(*text, path);
}

class LaneSoundDecoder::Stream final : public SoundDecodeStream {
 public:
  Stream(LaneSoundDecoder* owner, std::unique_ptr<audio::PcmDecoder> decoder)
      : owner_(owner), decoder_(std::move(decoder)) {}

  std::expected<bool, Error> DecodeStep(PcmPayload& pcm) override {
    const std::uint64_t start = SDL_GetTicksNS();
    bool done = false;
    // Chunks until the sound ends or the step's budget is spent.
    while (!done) {
      auto decoded = decoder_->Decode(pcm, kChunkFrames);
      if (!decoded) return decoded;
      done = *decoded;
      if (SDL_GetTicksNS() - start >= kStepBudgetNs) break;
    }
    const std::uint64_t took = SDL_GetTicksNS() - start;
    owner_->steps_.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t longest =
        owner_->longest_step_ns_.load(std::memory_order_relaxed);
    while (took > longest && !owner_->longest_step_ns_.compare_exchange_weak(
                                 longest, took, std::memory_order_relaxed)) {
    }
    owner_->last_role_.store(CurrentRole(), std::memory_order_relaxed);
    if (done) owner_->decoded_.fetch_add(1, std::memory_order_relaxed);
    return done;
  }

 private:
  LaneSoundDecoder* owner_;
  std::unique_ptr<audio::PcmDecoder> decoder_;
};

LaneSoundDecoder::LaneSoundDecoder(std::uint32_t sample_rate,
                                   std::uint32_t channels) noexcept
    : sample_rate_(sample_rate), channels_(channels) {}

std::expected<std::unique_ptr<SoundDecodeStream>, Error> LaneSoundDecoder::Open(
    std::vector<std::byte> encoded, std::string_view path) {
  auto decoder = audio::PcmDecoder::Open(std::move(encoded), sample_rate_,
                                         channels_, path);
  if (!decoder) return std::unexpected(std::move(decoder.error()));
  opened_.fetch_add(1, std::memory_order_relaxed);
  return std::make_unique<Stream>(this, std::move(*decoder));
}

SoundDecodeStats LaneSoundDecoder::Stats() const noexcept {
  SoundDecodeStats stats;
  stats.opened = opened_.load(std::memory_order_relaxed);
  stats.decoded = decoded_.load(std::memory_order_relaxed);
  stats.steps = steps_.load(std::memory_order_relaxed);
  stats.longest_step_ns = longest_step_ns_.load(std::memory_order_relaxed);
  stats.last_role = last_role_.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace se
