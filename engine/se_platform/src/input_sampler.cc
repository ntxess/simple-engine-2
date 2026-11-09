// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/input_sampler.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_power.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

#include "se_core/error.h"
#include "se_core/profile.h"
#include "se_core/thread_role.h"
#include "se_os/thread.h"

namespace se {

struct InputSampler::State {
  std::function<void()> poll;
  std::function<void(std::uint64_t ns)> delay;
  std::atomic<std::uint32_t> rate_hz{kSamplerHzOnAc};
  std::atomic<bool> park{false};
  // Rung (incremented, then notified) by SetParked and the stop.
  std::atomic<std::uint32_t> doorbell{0};
  std::atomic<bool> parked{false};
  std::atomic<std::uint64_t> polls{0};
  std::atomic<std::uint64_t> parks{0};
  std::atomic<std::uint64_t> unparks{0};
  std::atomic<std::uint64_t> poll_ns{0};
  std::atomic<std::uint64_t> poll_ns_max{0};
  std::atomic<std::uint64_t> wait_ns{0};

  void Ring() noexcept {
    doorbell.fetch_add(1, std::memory_order_acq_rel);
    doorbell.notify_all();
  }

  void Run(const std::stop_token& stop) {
    const std::stop_callback wake(stop, [this] { Ring(); });
    // Polls keep to a grid of whole periods: each wait aims at the next
    // grid point, early by the timer's measured overshoot (an average of
    // the last waits, at most half a period), so 1 kHz stays 1 kHz with a
    // timer that wakes late. A sampler that fell behind restarts the grid
    // from now; a park restarts it too.
    std::uint64_t deadline = 0;
    double lead_ns = 0.0;
    while (!stop.stop_requested()) {
      const std::uint32_t bell = doorbell.load(std::memory_order_acquire);
      if (park.load(std::memory_order_acquire)) {
        parks.fetch_add(1, std::memory_order_relaxed);
        parked.store(true, std::memory_order_release);
        // Returns once SetParked or the stop rang after |bell| was read.
        doorbell.wait(bell, std::memory_order_acquire);
        parked.store(false, std::memory_order_release);
        unparks.fetch_add(1, std::memory_order_relaxed);
        deadline = 0;
        continue;
      }
      const std::uint64_t start = SDL_GetTicksNS();
      {
        SE_PROFILE_ZONE("InputSampler poll");
        poll();
      }
      const std::uint64_t now = SDL_GetTicksNS();
      const std::uint64_t cost = now - start;
      polls.fetch_add(1, std::memory_order_relaxed);
      poll_ns.fetch_add(cost, std::memory_order_relaxed);
      if (cost > poll_ns_max.load(std::memory_order_relaxed)) {
        poll_ns_max.store(cost, std::memory_order_relaxed);
      }
      const std::uint32_t hz =
          std::max<std::uint32_t>(rate_hz.load(std::memory_order_relaxed), 1);
      const std::uint64_t period = 1'000'000'000ULL / hz;
      deadline = deadline == 0 ? now + period
                               : std::min(deadline + period, now + period);
      const std::uint64_t target = deadline > now ? deadline - now : 0;
      const auto lead = static_cast<std::uint64_t>(lead_ns);
      const std::uint64_t wait = target > lead ? target - lead : 0;
      if (wait > 0) delay(wait);
      const std::uint64_t woke = SDL_GetTicksNS();
      wait_ns.fetch_add(woke - now, std::memory_order_relaxed);
      if (wait > 0) {
        const double late =
            static_cast<double>(woke) - static_cast<double>(now + wait);
        const double overshoot =
            std::clamp(late, 0.0, static_cast<double>(period) * 0.5);
        lead_ns += (overshoot - lead_ns) / 8.0;
      }
    }
  }
};

std::expected<std::unique_ptr<InputSampler>, Error> InputSampler::Start(
    InputSamplerConfig config) {
  auto state = std::make_unique<State>();
  state->poll =
      config.poll ? std::move(config.poll) : [] { SDL_UpdateGamepads(); };
  state->delay = config.delay ? std::move(config.delay)
                              : [](std::uint64_t ns) { SDL_DelayNS(ns); };
  state->rate_hz.store(std::clamp<std::uint32_t>(config.rate_hz, 1, 2000));
  state->park.store(config.start_parked);
  std::unique_ptr<InputSampler> sampler(new InputSampler(std::move(state)));
  State* raw = sampler->state_.get();
  auto thread =
      StartThread(ThreadSpec{.role = ThreadRole::kInputSampler,
                             .name = "input-sampler",
                             .qos = Qos::kHigh},
                  [raw](const std::stop_token& stop) { raw->Run(stop); });
  if (!thread) return std::unexpected(std::move(thread.error()));
  sampler->thread_ = std::move(*thread);
  return sampler;
}

InputSampler::InputSampler(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

InputSampler::~InputSampler() { Stop(); }

void InputSampler::SetRate(std::uint32_t hz) noexcept {
  state_->rate_hz.store(std::clamp<std::uint32_t>(hz, 1, 2000),
                        std::memory_order_relaxed);
}

void InputSampler::SetParked(bool parked) noexcept {
  if (state_->park.exchange(parked, std::memory_order_acq_rel) == parked) {
    return;
  }
  state_->Ring();
}

void InputSampler::Stop() noexcept {
  if (!thread_.Joinable()) return;
  // The stop callback rings the doorbell, so a parked thread wakes.
  thread_.RequestStop();
  thread_.Join();
}

InputSamplerStats InputSampler::Stats() const noexcept {
  InputSamplerStats stats;
  stats.polls = state_->polls.load(std::memory_order_relaxed);
  stats.parks = state_->parks.load(std::memory_order_relaxed);
  stats.unparks = state_->unparks.load(std::memory_order_relaxed);
  stats.rate_hz = state_->rate_hz.load(std::memory_order_relaxed);
  stats.parked = state_->parked.load(std::memory_order_acquire);
  stats.poll_ns = state_->poll_ns.load(std::memory_order_relaxed);
  stats.poll_ns_max = state_->poll_ns_max.load(std::memory_order_relaxed);
  stats.wait_ns = state_->wait_ns.load(std::memory_order_relaxed);
  return stats;
}

PowerWatch::PowerWatch(std::function<bool()> on_battery)
    : query_(on_battery ? std::move(on_battery)
                        : [] { return OnBatteryPower(); }) {}

std::optional<std::uint32_t> PowerWatch::Update(std::uint64_t now_ns) {
  if (known_ && now_ns < next_ns_) return std::nullopt;
  next_ns_ = now_ns + kIntervalNs;
  const bool battery = query_();
  if (known_ && battery == on_battery_) return std::nullopt;
  known_ = true;
  on_battery_ = battery;
  return SamplerRateFor(battery);
}

bool OnBatteryPower() {
  SE_ASSERT_MAIN_THREAD();
  return SDL_GetPowerInfo(nullptr, nullptr) == SDL_POWERSTATE_ON_BATTERY;
}

}  // namespace se
