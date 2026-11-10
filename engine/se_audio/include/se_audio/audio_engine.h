// Copyright 2026 The simple-engine-2 Authors.
//
// The audio engine (docs/milestone-specs.md M8; docs/design/mt-design.md
// "Audio And Assets" (Audio), "Platform" (miniaudio's rules); ADR-076).
// Main owns the AudioEngine, a miniaudio ma_engine, and is the only thread
// that calls ma_engine and ma_sound functions; miniaudio mixes on its own
// device thread. miniaudio is confined to se_audio (the link graph and
// tools/checks/check_includes.py).
//
// Output. kDevice opens the default playback device; when that fails (no
// device, as on CI machines and VMs) the engine runs on miniaudio's null
// backend, whose thread mixes in real time and plays nothing, and says so
// (FellBack, FallbackReason). kNull asks for the null backend. kNoDevice
// creates no device: frames are pulled with ReadFrames (tests, offline
// checks), and whoever pulls them is the mixer.
//
// Groups. Music and Effects, each a ma_sound_group with its own volume
// (config/config.json "sound-settings", 0-100 as 0-1) and voice cap.
//
// Voices. A voice is a fixed record (kMaxVoices of them, never moved): its
// ma_sound, the ma_audio_buffer that reads its PCM in place, the PCM
// reference, its group, scope, cosmetic flag and start serial. A voice id
// is its slot and a generation, so a stale id names nothing.
//   - Play starts a voice at an absolute engine frame
//     (ma_sound_set_start_time_in_pcm_frames), or at once with 0. A group
//     at its cap steals its oldest voice first.
//   - When a sound ends, miniaudio's thread calls the end callback, which
//     writes the voice id into an SPSC ring of kMaxVoices entries and never
//     waits (a full ring is counted; the sweep still finds the voice).
//   - Sweep(), once per frame on main, drains the ring and then
//     uninitialises every voice whose ma_sound_at_end is true.
//   - A voice's PCM reference is dropped only after its ma_sound_uninit has
//     returned (ma_sound_uninit waits for the mixer: it is the fence).
//   - ReleaseScope stops and uninitialises the scope's voices first; Stop
//     does one voice.
//   - SetSound replaces a sound's PCM: new voices get the new PCM, voices
//     already playing keep the old one until they end.
// Destruction (shutdown step 7): every voice stopped and uninitialised,
// ma_engine_uninit (which joins miniaudio's device thread), then the PCM.
//
// Main thread only, except ReadFrames (the mixer of a kNoDevice engine) and
// TimeFrames.

#ifndef SE_AUDIO_AUDIO_ENGINE_H_
#define SE_AUDIO_AUDIO_ENGINE_H_

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "se_audio/pcm.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

namespace se::audio {

enum class Output : std::uint8_t { kDevice, kNull, kNoDevice };
// What the engine runs on.
enum class Backend : std::uint8_t { kNoDevice, kDevice, kNull };
[[nodiscard]] std::string_view BackendName(Backend backend) noexcept;

enum class Group : std::uint8_t { kMusic, kEffects };
inline constexpr int kGroupCount = 2;
[[nodiscard]] std::string_view GroupName(Group group) noexcept;

inline constexpr std::uint32_t kMaxVoices = 256;

struct EngineConfig {
  Output output = Output::kDevice;
  std::uint32_t sample_rate = 48000;
  std::uint32_t channels = 2;
  float master_volume = 1.0F;
  // By Group.
  std::array<float, kGroupCount> group_volume{1.0F, 1.0F};
  // By Group; their sum is at most kMaxVoices.
  std::array<std::uint32_t, kGroupCount> voice_cap{32, 224};
  // The device's period; 0: miniaudio's default.
  std::uint32_t period_ms = 0;
  // Tests: the device fails to open (the null-backend fallback).
  bool fail_device_for_test = false;
};

// A voice: its slot and generation. 0 is no voice.
struct VoiceId {
  std::uint32_t value = 0;
  [[nodiscard]] bool IsNull() const noexcept { return value == 0; }
  friend bool operator==(VoiceId, VoiceId) = default;
};

// Voices that belong to something with a lifetime (a World); 0: none.
using ScopeId = std::uint32_t;

struct PlayRequest {
  std::uint32_t sound = 0;
  Group group = Group::kEffects;
  bool cosmetic = false;
  ScopeId scope = 0;
  // The engine frame (TimeFrames' clock) to start at; 0, or a frame
  // already mixed: at once.
  std::uint64_t start_frame = 0;
  // -1 (left) to 1 (right).
  float pan = 0.0F;
  float volume = 1.0F;
};

struct EngineStats {
  std::uint64_t voices_started = 0;
  std::uint64_t voices_stolen = 0;
  std::uint64_t ended_by_ring = 0;
  std::uint64_t ended_by_sweep = 0;
  std::uint64_t voices_stopped = 0;
  std::uint64_t scope_releases = 0;
  std::uint64_t end_ring_drops = 0;
  std::uint64_t unknown_sounds = 0;
  std::uint64_t sounds_set = 0;
  std::uint32_t voices_active = 0;
};

class AudioEngine {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<AudioEngine>, Error>
  Create(const EngineConfig& config) SE_REQUIRES(g_main);

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;
  ~AudioEngine();

  [[nodiscard]] Backend GetBackend() const noexcept;
  // kDevice was asked for and the null backend runs instead.
  [[nodiscard]] bool FellBack() const noexcept;
  [[nodiscard]] const std::string& FallbackReason() const noexcept;
  [[nodiscard]] std::uint32_t SampleRate() const noexcept;
  [[nodiscard]] std::uint32_t Channels() const noexcept;

  // Adds or replaces sound |key|. Main.
  void SetSound(std::uint32_t key, PcmRef pcm) SE_REQUIRES(g_main);
  [[nodiscard]] PcmRef Sound(std::uint32_t key) const SE_REQUIRES(g_main);

  // Starts a voice (main). An unknown sound is an Error (counted).
  std::expected<VoiceId, Error> Play(const PlayRequest& request)
      SE_REQUIRES(g_main);
  // Stops and uninitialises a voice; false when it already ended.
  bool Stop(VoiceId voice) SE_REQUIRES(g_main);
  // Stops and uninitialises every voice of |scope|; returns how many.
  std::uint32_t ReleaseScope(ScopeId scope) SE_REQUIRES(g_main);
  void StopAll() SE_REQUIRES(g_main);
  // Drains the end ring and uninitialises voices at their end (main, every
  // iterate).
  void Sweep() SE_REQUIRES(g_main);
  [[nodiscard]] bool Playing(VoiceId voice) const SE_REQUIRES(g_main);
  [[nodiscard]] std::uint32_t ActiveVoices(Group group) const
      SE_REQUIRES(g_main);

  // The engine's clock: PCM frames mixed so far (any thread).
  [[nodiscard]] std::uint64_t TimeFrames() const noexcept;
  void SetMasterVolume(float volume) SE_REQUIRES(g_main);
  void SetGroupVolume(Group group, float volume) SE_REQUIRES(g_main);

  // kNoDevice only: mixes out.size() / Channels() frames into |out|;
  // returns the frames mixed. The thread that calls it is the mixer.
  std::uint64_t ReadFrames(std::span<float> out);

  // Main (the end ring's drop count is the mixer's, read atomically).
  [[nodiscard]] EngineStats Stats() const SE_REQUIRES(g_main);

  struct Impl;

 private:
  explicit AudioEngine(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace se::audio

#endif  // SE_AUDIO_AUDIO_ENGINE_H_
