// Copyright 2026 The simple-engine-2 Authors.
//
// AudioBridge: the SimDriver's AudioSink in the runtime (docs/milestone-
// specs.md M8; docs/design/mt-design.md "Audio And Assets", "Scheduling";
// ADR-077). After each tick it drains the World's AudioEvents (se_sim/
// audio_events.h) and starts each sound on the AudioEngine at
// max(now, tick anchor + D):
//
//   - the anchor is the tick's input window end (TickInline passes it);
//   - D = B + T/2 comes from the frame's plan and the pacer: B the plan's
//     budget (StepPlan::budget_b), T the display's vblank period; 13.8 ms
//     at 60 Hz Paced Locked. It is the time from a tick's window end to its
//     frame at mid-screen, so the two ticks of a 60 Hz frame sound 8.33 ms
//     apart;
//   - the platform time (SDL_GetTicksNS) maps to the engine's PCM clock
//     through a PcmClock recalibrated every frame (BeginFrame);
//   - a sound whose start has passed is late: gameplay sounds play at once,
//     cosmetic ones more than 100 ms late are dropped (counted); a start
//     further ahead than now + D (an anchor from a synthetic clock) starts
//     at now + D;
//   - each group's voice cap steals its oldest voice (AudioEngine).
//
// Each World gets a voice scope the first time it is drained; the scope's
// token lives in its AudioEvents, so destroying the World (a state's exit,
// shutdown step 6) stops and uninitialises its voices. Without an engine
// (--audio none) events are dropped and counted. Main thread only.

#ifndef SE_RUNTIME_AUDIO_BRIDGE_H_
#define SE_RUNTIME_AUDIO_BRIDGE_H_

#include <cstdint>
#include <functional>
#include <memory>

#include "se_audio/audio_engine.h"
#include "se_audio/pcm_clock.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/world.h"
#include "se_sim/audio_events.h"
#include "se_sim/sim_driver.h"

namespace se {

// Cosmetic sounds later than this are dropped.
inline constexpr std::uint64_t kCosmeticDropNs = 100'000'000;

// D = B + T/2 in nanoseconds.
[[nodiscard]] std::uint64_t AudioDelayNs(double budget_b_sec,
                                         double vblank_period_sec) noexcept;

// When one event starts: |play| false for a dropped cosmetic sound;
// start_frame 0 for "at once".
struct AudioStart {
  bool play = true;
  bool late = false;
  std::uint64_t lateness_ns = 0;
  std::uint64_t start_frame = 0;
};
[[nodiscard]] AudioStart ScheduleAudio(std::uint64_t anchor_ns,
                                       std::uint64_t delay_ns,
                                       std::uint64_t now_ns, bool cosmetic,
                                       const audio::PcmClock& clock) noexcept;

struct AudioBridgeStats {
  std::uint64_t events = 0;
  std::uint64_t played = 0;
  // Played at once because their start had passed.
  std::uint64_t late = 0;
  std::uint64_t cosmetic_dropped = 0;
  // Unknown sounds, or the engine refused (counted, logged once each).
  std::uint64_t failed = 0;
  // Drained without an engine.
  std::uint64_t no_engine = 0;
  std::uint64_t scopes = 0;
};

class AudioBridge final : public AudioSink {
 public:
  // |engine| may be nullptr (no audio). |now| is the platform clock
  // (default SDL_GetTicksNS).
  explicit AudioBridge(audio::AudioEngine* engine,
                       std::function<std::uint64_t()> now = {});
  AudioBridge(const AudioBridge&) = delete;
  AudioBridge& operator=(const AudioBridge&) = delete;
  ~AudioBridge() override;

  // Each frame before its ticks: recalibrates the PCM clock at |now_ns| and
  // sets D.
  void BeginFrame(std::uint64_t now_ns, std::uint64_t delay_ns)
      SE_REQUIRES(g_main);
  // Pan: the listener's x and the half width that pans fully.
  void SetListener(float center_x, float half_width) noexcept;

  void Drain(World& world, std::uint64_t tick_window_end_ns)
      SE_REQUIRES(g_sim_owner) override;

  // Each iterate: the engine's sweep (the end ring, finished voices).
  void Sweep() SE_REQUIRES(g_main);

  [[nodiscard]] const AudioBridgeStats& Stats() const noexcept {
    return stats_;
  }
  [[nodiscard]] std::uint64_t DelayNs() const noexcept { return delay_ns_; }
  [[nodiscard]] const audio::PcmClock& Clock() const noexcept { return clock_; }

  struct Core;

 private:
  std::shared_ptr<Core> core_;
  std::function<std::uint64_t()> now_;
  audio::PcmClock clock_;
  std::uint64_t delay_ns_ = 0;
  float listener_x_ = 0.0F;
  float listener_half_width_ = 0.0F;
  AudioBridgeStats stats_;
};

}  // namespace se

#endif  // SE_RUNTIME_AUDIO_BRIDGE_H_
