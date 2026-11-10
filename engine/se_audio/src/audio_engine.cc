// Copyright 2026 The simple-engine-2 Authors.

#include "se_audio/audio_engine.h"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "se_audio/pcm.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/spsc_ring.h"
#include "se_core/thread_role.h"

namespace se::audio {
namespace {

constexpr std::uint32_t kSlotBits = 8;
constexpr std::uint32_t kSlotMask = (1U << kSlotBits) - 1;
static_assert(kMaxVoices == 1U << kSlotBits, "a voice id holds its slot");

std::unexpected<Error> MaError(std::string_view what, ma_result result) {
  return Fail(ErrorCode::kIo,
              std::format("{}: {} ({})", what, ma_result_description(result),
                          static_cast<int>(result)));
}

}  // namespace

std::string_view BackendName(Backend backend) noexcept {
  switch (backend) {
    case Backend::kNoDevice:
      return "no device";
    case Backend::kDevice:
      return "device";
    case Backend::kNull:
      return "null backend";
  }
  return "?";
}

std::string_view GroupName(Group group) noexcept {
  return group == Group::kMusic ? "music" : "effects";
}

// One voice record. Never moved: miniaudio keeps pointers to the ma_sound
// and the ma_audio_buffer, and the end callback to the record.
struct Voice {
  ma_sound sound{};
  ma_audio_buffer buffer{};
  PcmRef pcm;
  // Written on main before the sound starts and after it is uninitialised;
  // the end callback reads |id| and |owner| in between.
  std::uint32_t id = 0;
  AudioEngine::Impl* owner = nullptr;
  std::uint32_t generation = 0;
  bool active = false;
  bool cosmetic = false;
  Group group = Group::kEffects;
  ScopeId scope = 0;
  std::uint64_t serial = 0;
};

struct AudioEngine::Impl {
  explicit Impl(const EngineConfig& engine_config)
      : config(engine_config),
        voices(std::make_unique<Voice[]>(kMaxVoices)),
        end_ring(kMaxVoices) {}

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  ~Impl() {
    // Shutdown step 7: voices, then the engine (joins the device thread),
    // then the PCM.
    if (engine_ready) {
      for (std::uint32_t slot = 0; slot < kMaxVoices; ++slot) {
        if (voices[slot].active) Release(voices[slot]);
      }
      for (std::size_t g = 0; g < groups.size(); ++g) {
        if (group_ready[g]) ma_sound_group_uninit(&groups[g]);
      }
      ma_engine_uninit(&engine);
    }
    if (resource_manager_ready) ma_resource_manager_uninit(&resource_manager);
    if (context_ready) ma_context_uninit(&context);
    sounds.clear();
  }

  // The end callback, on miniaudio's mixing thread: never waits, never
  // allocates.
  static void OnSoundEnd(void* user, ma_sound* /*sound*/) {
    const auto* voice = static_cast<const Voice*>(user);
    Impl* impl = voice->owner;
    if (!impl->end_ring.TryPush(voice->id)) {
      impl->end_ring_drops.fetch_add(1, std::memory_order_relaxed);
    }
  }

  std::expected<void, Error> Init() {
    ma_resource_manager_config rm_config = ma_resource_manager_config_init();
    // The engine loads nothing through it: no job threads.
    rm_config.flags |= MA_RESOURCE_MANAGER_FLAG_NO_THREADING;
    rm_config.jobThreadCount = 0;
    if (const ma_result result =
            ma_resource_manager_init(&rm_config, &resource_manager);
        result != MA_SUCCESS) {
      return MaError("ma_resource_manager_init", result);
    }
    resource_manager_ready = true;

    if (config.output == Output::kNoDevice) {
      if (auto ok = InitEngine(nullptr, true); !ok) {
        return ok;
      }
      backend = Backend::kNoDevice;
    } else if (config.output == Output::kNull) {
      if (auto ok = InitNullEngine(); !ok) {
        return ok;
      }
    } else {
      std::expected<void, Error> device =
          config.fail_device_for_test
              ? std::expected<void, Error>(
                    Fail(ErrorCode::kIo, "no device (fail_device_for_test)"))
              : InitEngine(nullptr, false);
      if (device) {
        backend = Backend::kDevice;
      } else {
        fell_back = true;
        fallback_reason = device.error().message;
        if (auto ok = InitNullEngine(); !ok) {
          return ok;
        }
      }
    }

    for (std::size_t g = 0; g < groups.size(); ++g) {
      if (const ma_result result =
              ma_sound_group_init(&engine, 0, nullptr, &groups[g]);
          result != MA_SUCCESS) {
        return MaError("ma_sound_group_init", result);
      }
      group_ready[g] = true;
      ma_sound_group_set_volume(&groups[g], config.group_volume[g]);
    }
    ma_engine_set_volume(&engine, config.master_volume);
    for (std::uint32_t slot = 0; slot < kMaxVoices; ++slot) {
      voices[slot].owner = this;
    }
    return {};
  }

  std::expected<void, Error> InitNullEngine() {
    const std::array<ma_backend, 1> backends = {ma_backend_null};
    if (const ma_result result =
            ma_context_init(backends.data(), 1, nullptr, &context);
        result != MA_SUCCESS) {
      return MaError("ma_context_init (null backend)", result);
    }
    context_ready = true;
    if (auto ok = InitEngine(&context, false); !ok) {
      return ok;
    }
    backend = Backend::kNull;
    return {};
  }

  std::expected<void, Error> InitEngine(ma_context* with_context,
                                        bool no_device) {
    ma_engine_config engine_config = ma_engine_config_init();
    engine_config.pResourceManager = &resource_manager;
    engine_config.pContext = with_context;
    engine_config.channels = config.channels;
    engine_config.sampleRate = config.sample_rate;
    engine_config.periodSizeInMilliseconds = config.period_ms;
    engine_config.noDevice = no_device ? MA_TRUE : MA_FALSE;
    if (const ma_result result = ma_engine_init(&engine_config, &engine);
        result != MA_SUCCESS) {
      return MaError(
          no_device ? "ma_engine_init (no device)" : "ma_engine_init", result);
    }
    engine_ready = true;
    return {};
  }

  [[nodiscard]] std::uint32_t Active(Group group) const {
    std::uint32_t count = 0;
    for (std::uint32_t slot = 0; slot < kMaxVoices; ++slot) {
      if (voices[slot].active && voices[slot].group == group) ++count;
    }
    return count;
  }

  // ma_sound_uninit waits for the mixer: after it, nothing reads the PCM.
  void Release(Voice& voice) {
    ma_sound_uninit(&voice.sound);
    ma_audio_buffer_uninit(&voice.buffer);
    voice.pcm.reset();
    voice.active = false;
    voice.id = 0;
  }

  Voice* Find(VoiceId id) const {
    Voice& voice = voices[id.value & kSlotMask];
    return voice.active && voice.id == id.value ? &voice : nullptr;
  }

  EngineConfig config;
  Backend backend = Backend::kNoDevice;
  bool fell_back = false;
  std::string fallback_reason;
  ma_resource_manager resource_manager{};
  bool resource_manager_ready = false;
  ma_context context{};
  bool context_ready = false;
  ma_engine engine{};
  bool engine_ready = false;
  std::array<ma_sound_group, kGroupCount> groups{};
  std::array<bool, kGroupCount> group_ready{};
  std::unique_ptr<Voice[]> voices;
  SpscRing<std::uint32_t> end_ring;
  std::atomic<std::uint64_t> end_ring_drops{0};
  std::unordered_map<std::uint32_t, PcmRef> sounds;
  std::uint64_t next_serial = 1;
  EngineStats stats;
};

std::expected<std::unique_ptr<AudioEngine>, Error> AudioEngine::Create(
    const EngineConfig& config) {
  SE_ASSERT_MAIN_THREAD();
  if (config.channels == 0 || config.sample_rate == 0) {
    return Fail(ErrorCode::kInvalidArgument,
                "audio: the channel count and sample rate must be set");
  }
  if (config.voice_cap[0] + config.voice_cap[1] > kMaxVoices) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("audio: the group voice caps sum to more than {}",
                            kMaxVoices));
  }
  auto impl = std::make_unique<Impl>(config);
  if (auto ok = impl->Init(); !ok) {
    return std::unexpected(ok.error());
  }
  if (impl->fell_back) {
    SE_LOG_WARN(
        "audio: the playback device did not open ({}); using the "
        "null backend",
        impl->fallback_reason);
  }
  SE_LOG_INFO("audio: {} at {} Hz, {} channels", BackendName(impl->backend),
              ma_engine_get_sample_rate(&impl->engine),
              ma_engine_get_channels(&impl->engine));
  return std::unique_ptr<AudioEngine>(new AudioEngine(std::move(impl)));
}

AudioEngine::AudioEngine(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

AudioEngine::~AudioEngine() {
  if (impl_) SE_ASSERT_MAIN_THREAD();
}

Backend AudioEngine::GetBackend() const noexcept { return impl_->backend; }
bool AudioEngine::FellBack() const noexcept { return impl_->fell_back; }
const std::string& AudioEngine::FallbackReason() const noexcept {
  return impl_->fallback_reason;
}
std::uint32_t AudioEngine::SampleRate() const noexcept {
  return ma_engine_get_sample_rate(&impl_->engine);
}
std::uint32_t AudioEngine::Channels() const noexcept {
  return ma_engine_get_channels(&impl_->engine);
}

void AudioEngine::SetSound(std::uint32_t key, PcmRef pcm) {
  SE_ASSERT_MAIN_THREAD();
  ++impl_->stats.sounds_set;
  impl_->sounds[key] = std::move(pcm);
}

PcmRef AudioEngine::Sound(std::uint32_t key) const {
  SE_ASSERT_MAIN_THREAD();
  const auto it = impl_->sounds.find(key);
  return it != impl_->sounds.end() ? it->second : nullptr;
}

std::expected<VoiceId, Error> AudioEngine::Play(const PlayRequest& request) {
  SE_ASSERT_MAIN_THREAD();
  Impl& impl = *impl_;
  const auto it = impl.sounds.find(request.sound);
  if (it == impl.sounds.end() || it->second == nullptr ||
      it->second->frames == 0) {
    ++impl.stats.unknown_sounds;
    return Fail(ErrorCode::kNotFound,
                std::format("audio: no sound {:#010x}", request.sound));
  }
  const auto g = static_cast<std::size_t>(request.group);
  // A group at its cap gives up its oldest voice.
  if (impl.Active(request.group) >= impl.config.voice_cap[g]) {
    Voice* oldest = nullptr;
    for (std::uint32_t slot = 0; slot < kMaxVoices; ++slot) {
      Voice& voice = impl.voices[slot];
      if (voice.active && voice.group == request.group &&
          (oldest == nullptr || voice.serial < oldest->serial)) {
        oldest = &voice;
      }
    }
    if (oldest == nullptr) {
      return Fail(ErrorCode::kInvalidArgument,
                  std::format("audio: the {} group has no voices",
                              GroupName(request.group)));
    }
    impl.Release(*oldest);
    ++impl.stats.voices_stolen;
  }
  Voice* unused = nullptr;
  for (std::uint32_t slot = 0; slot < kMaxVoices && unused == nullptr; ++slot) {
    if (!impl.voices[slot].active) unused = &impl.voices[slot];
  }
  if (unused == nullptr) {
    return Fail(ErrorCode::kInvalidArgument, "audio: every voice is in use");
  }
  Voice& voice = *unused;
  const auto slot = static_cast<std::uint32_t>(unused - impl.voices.get());
  voice.generation = (voice.generation + 1) & (UINT32_MAX >> kSlotBits);
  if (voice.generation == 0) voice.generation = 1;
  const PcmRef& pcm = it->second;
  ma_audio_buffer_config buffer_config = ma_audio_buffer_config_init(
      ma_format_f32, pcm->channels, pcm->frames, pcm->samples.data(), nullptr);
  buffer_config.sampleRate = pcm->sample_rate;
  // The buffer reads the PCM in place (no copy).
  if (const ma_result result =
          ma_audio_buffer_init(&buffer_config, &voice.buffer);
      result != MA_SUCCESS) {
    return MaError("ma_audio_buffer_init", result);
  }
  ma_sound_config sound_config = ma_sound_config_init_2(&impl.engine);
  sound_config.pDataSource = &voice.buffer;
  sound_config.pInitialAttachment = &impl.groups[g];
  sound_config.flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;
  sound_config.endCallback = &Impl::OnSoundEnd;
  sound_config.pEndCallbackUserData = &voice;
  voice.id = (voice.generation << kSlotBits) | slot;
  if (const ma_result result =
          ma_sound_init_ex(&impl.engine, &sound_config, &voice.sound);
      result != MA_SUCCESS) {
    ma_audio_buffer_uninit(&voice.buffer);
    voice.id = 0;
    return MaError("ma_sound_init_ex", result);
  }
  voice.pcm = pcm;
  voice.active = true;
  voice.cosmetic = request.cosmetic;
  voice.group = request.group;
  voice.scope = request.scope;
  voice.serial = impl.next_serial++;
  ma_sound_set_pan(&voice.sound, std::clamp(request.pan, -1.0F, 1.0F));
  ma_sound_set_volume(&voice.sound, std::max(request.volume, 0.0F));
  if (request.start_frame > 0) {
    ma_sound_set_start_time_in_pcm_frames(&voice.sound, request.start_frame);
  }
  if (const ma_result result = ma_sound_start(&voice.sound);
      result != MA_SUCCESS) {
    impl.Release(voice);
    return MaError("ma_sound_start", result);
  }
  ++impl.stats.voices_started;
  return VoiceId{voice.id};
}

bool AudioEngine::Stop(VoiceId voice) {
  SE_ASSERT_MAIN_THREAD();
  Voice* found = impl_->Find(voice);
  if (found == nullptr) return false;
  impl_->Release(*found);
  ++impl_->stats.voices_stopped;
  return true;
}

std::uint32_t AudioEngine::ReleaseScope(ScopeId scope) {
  SE_ASSERT_MAIN_THREAD();
  std::uint32_t released = 0;
  for (std::uint32_t slot = 0; slot < kMaxVoices; ++slot) {
    Voice& voice = impl_->voices[slot];
    if (!voice.active || voice.scope != scope) continue;
    impl_->Release(voice);
    ++released;
  }
  ++impl_->stats.scope_releases;
  impl_->stats.voices_stopped += released;
  return released;
}

void AudioEngine::StopAll() {
  SE_ASSERT_MAIN_THREAD();
  for (std::uint32_t slot = 0; slot < kMaxVoices; ++slot) {
    Voice& voice = impl_->voices[slot];
    if (!voice.active) continue;
    impl_->Release(voice);
    ++impl_->stats.voices_stopped;
  }
}

void AudioEngine::Sweep() {
  SE_ASSERT_MAIN_THREAD();
  Impl& impl = *impl_;
  impl.end_ring.PopAll([&impl](std::uint32_t id) {
    Voice* voice = impl.Find(VoiceId{id});
    if (voice == nullptr) return;  // already swept or stopped
    impl.Release(*voice);
    ++impl.stats.ended_by_ring;
  });
  for (std::uint32_t slot = 0; slot < kMaxVoices; ++slot) {
    Voice& voice = impl.voices[slot];
    if (voice.active && ma_sound_at_end(&voice.sound) == MA_TRUE) {
      impl.Release(voice);
      ++impl.stats.ended_by_sweep;
    }
  }
}

bool AudioEngine::Playing(VoiceId voice) const {
  SE_ASSERT_MAIN_THREAD();
  return impl_->Find(voice) != nullptr;
}

std::uint32_t AudioEngine::ActiveVoices(Group group) const {
  SE_ASSERT_MAIN_THREAD();
  return impl_->Active(group);
}

std::uint64_t AudioEngine::TimeFrames() const noexcept {
  return ma_engine_get_time_in_pcm_frames(&impl_->engine);
}

void AudioEngine::SetMasterVolume(float volume) {
  SE_ASSERT_MAIN_THREAD();
  ma_engine_set_volume(&impl_->engine, std::max(volume, 0.0F));
}

void AudioEngine::SetGroupVolume(Group group, float volume) {
  SE_ASSERT_MAIN_THREAD();
  ma_sound_group_set_volume(&impl_->groups[static_cast<std::size_t>(group)],
                            std::max(volume, 0.0F));
}

std::uint64_t AudioEngine::ReadFrames(std::span<float> out) {
  if (impl_->backend != Backend::kNoDevice) return 0;
  const std::uint32_t channels = ma_engine_get_channels(&impl_->engine);
  const std::uint64_t frames = out.size() / channels;
  ma_uint64 read = 0;
  if (ma_engine_read_pcm_frames(&impl_->engine, out.data(), frames, &read) !=
      MA_SUCCESS) {
    return 0;
  }
  return read;
}

EngineStats AudioEngine::Stats() const {
  SE_ASSERT_MAIN_THREAD();
  EngineStats stats = impl_->stats;
  stats.end_ring_drops = impl_->end_ring_drops.load(std::memory_order_relaxed);
  stats.voices_active = 0;
  for (std::uint32_t slot = 0; slot < kMaxVoices; ++slot) {
    if (impl_->voices[slot].active) ++stats.voices_active;
  }
  return stats;
}

}  // namespace se::audio
