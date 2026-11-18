// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/audio_bridge.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "se_audio/audio_engine.h"
#include "se_audio/pcm_clock.h"
#include "se_core/log.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_sim/audio_events.h"

namespace se {

// What the voice scopes of the Worlds share with the bridge: the engine
// while the bridge lives.
struct AudioBridge::Core {
  audio::AudioEngine* engine = nullptr;
  audio::ScopeId next_scope = 1;
};

namespace {

// Lives in a World's AudioEvents; destroying the World releases the voices
// of its scope (on main: Worlds are destroyed there).
struct ScopeToken {
  ScopeToken(std::weak_ptr<AudioBridge::Core> bridge, audio::ScopeId id)
      : core(std::move(bridge)), scope(id) {}
  ScopeToken(const ScopeToken&) = delete;
  ScopeToken& operator=(const ScopeToken&) = delete;
  ~ScopeToken() {
    const std::shared_ptr<AudioBridge::Core> alive = core.lock();
    if (alive == nullptr || alive->engine == nullptr) return;
    AssertMainThread();
    alive->engine->ReleaseScope(scope);
  }
  std::weak_ptr<AudioBridge::Core> core;
  audio::ScopeId scope;
};

audio::Group GroupOf(AudioGroup group) noexcept {
  return group == AudioGroup::kMusic ? audio::Group::kMusic
                                     : audio::Group::kEffects;
}

}  // namespace

std::uint64_t AudioDelayNs(double budget_b_sec,
                           double vblank_period_sec) noexcept {
  const double d =
      std::max(budget_b_sec, 0.0) + std::max(vblank_period_sec, 0.0) * 0.5;
  return static_cast<std::uint64_t>(std::llround(d * 1e9));
}

std::uint64_t PlanAudioDelayNs(const StepPlan& plan, double vblank_period_sec,
                               double frame_period_sec,
                               double tick_dt_sec) noexcept {
  const double vblank = std::max(vblank_period_sec, 0.0);
  const double frame = std::max(frame_period_sec, 0.0);
  const double dt = std::max(tick_dt_sec, 0.0);
  double lead = 0.0;
  if (plan.cadence == Cadence::kLocked && plan.locked_k >= 1) {
    // k equal windows split the frame: the first ends (k - 1) / k of it
    // before the frame starts.
    const double k = static_cast<double>(plan.locked_k);
    lead = frame * (k - 1.0) / k;
  } else if (dt > 0.0) {
    // A tick's window ends `remainder + (steps - 1 - i) * dt` before the
    // frame starts, with remainder < dt and at most ceil(P / dt) steps in a
    // frame that is not catching up. The epsilon keeps P = n * dt at n.
    lead = std::max(1.0, std::ceil(frame / dt - 1e-6)) * dt;
  }
  const double d = std::max(plan.budget_b, 0.0) + std::max(vblank * 0.5, lead);
  return static_cast<std::uint64_t>(std::llround(d * 1e9));
}

AudioStart ScheduleAudio(std::uint64_t anchor_ns, std::uint64_t delay_ns,
                         std::uint64_t now_ns, bool cosmetic,
                         const audio::PcmClock& clock) noexcept {
  AudioStart start;
  // A start further ahead than now + D comes from a clock that runs ahead
  // of the platform's (a synthetic one): no later than now + D.
  const std::uint64_t target =
      std::min(anchor_ns + delay_ns, now_ns + delay_ns);
  if (target >= now_ns) {
    start.start_frame = clock.FrameAt(target);
    return start;
  }
  start.late = true;
  start.lateness_ns = now_ns - target;
  if (cosmetic && start.lateness_ns > kCosmeticDropNs) start.play = false;
  return start;
}

AudioBridge::AudioBridge(audio::AudioEngine* engine,
                         std::function<std::uint64_t()> now)
    : core_(std::make_shared<Core>()),
      now_(now ? std::move(now) : [] { return SDL_GetTicksNS(); }),
      clock_(engine != nullptr ? engine->SampleRate() : 48000) {
  core_->engine = engine;
}

AudioBridge::~AudioBridge() {
  // Scopes of Worlds that outlive the bridge no longer reach the engine.
  core_->engine = nullptr;
}

void AudioBridge::BeginFrame(std::uint64_t now_ns, std::uint64_t delay_ns) {
  SE_ASSERT_MAIN_THREAD();
  delay_ns_ = delay_ns;
  if (core_->engine != nullptr) {
    clock_.Calibrate(now_ns, core_->engine->TimeFrames());
  }
}

void AudioBridge::SetListener(float center_x, float half_width) noexcept {
  listener_x_ = center_x;
  listener_half_width_ = half_width;
}

void AudioBridge::Drain(World& world, std::uint64_t tick_window_end_ns) {
  AssertMainThread();
  // The drain ends the tick: a Play World's gate opens for it.
  const MutationGate gate = world.OpenGate(GateReason::kTick);
  AudioEvents* events = FindAudioEvents(world);
  if (events == nullptr || events->Empty()) return;
  stats_.events += events->Events().size();
  audio::AudioEngine* engine = core_->engine;
  if (engine == nullptr) {
    stats_.no_engine += events->Events().size();
    events->Clear();
    return;
  }
  if (events->OwnerToken() == nullptr) {
    events->SetOwnerToken(
        std::make_shared<ScopeToken>(core_, core_->next_scope++));
    ++stats_.scopes;
  }
  const auto* token =
      static_cast<const ScopeToken*>(events->OwnerToken().get());
  if (!clock_.Calibrated()) clock_.Calibrate(now_(), engine->TimeFrames());
  const std::uint64_t now_ns = now_();
  for (const AudioEvent& event : events->Events()) {
    const AudioStart start = ScheduleAudio(tick_window_end_ns, delay_ns_,
                                           now_ns, event.cosmetic, clock_);
    if (!start.play) {
      ++stats_.cosmetic_dropped;
      continue;
    }
    audio::PlayRequest request;
    request.sound = event.sound;
    request.group = GroupOf(event.group);
    request.cosmetic = event.cosmetic;
    request.scope = token->scope;
    request.start_frame = start.start_frame;
    if (event.positional && listener_half_width_ > 0.0F) {
      request.pan = std::clamp(
          (event.position.x - listener_x_) / listener_half_width_, -1.0F, 1.0F);
    }
    auto voice = engine->Play(request);
    if (!voice) {
      if (stats_.failed == 0) {
        SE_LOG_WARN("audio event not played: {}", voice.error().message);
      }
      ++stats_.failed;
      continue;
    }
    ++stats_.played;
    if (start.late) ++stats_.late;
  }
  events->Clear();
}

void AudioBridge::Sweep() {
  SE_ASSERT_MAIN_THREAD();
  if (core_->engine != nullptr) core_->engine->Sweep();
}

}  // namespace se
