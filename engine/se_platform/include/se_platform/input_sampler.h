// Copyright 2026 The simple-engine-2 Authors.
//
// The input sampler (docs/design/mt-design.md "input sampler", red team
// P10; docs/milestone-specs.md M8; ADR-075). One engine thread, role
// kInputSampler, HighQoS, started with se::StartThread. It calls
// SDL_UpdateGamepads (documented safe from any thread) every period:
// 1000 Hz on AC power, 250 Hz on battery (main reads SDL_GetPowerInfo
// every 5 s through PowerWatch and publishes the rate with SetRate), and
// 20 Hz while no gamepad is open (device detection only: SDL finds new pads
// through the same update; ADR-090). With
// SDL_HINT_AUTO_UPDATE_JOYSTICKS=0 (SdlContext sets it) nothing else
// updates gamepads, so pad events carry stamps of about one period. It
// waits with SDL_DelayNS (a high-resolution waitable timer on Windows) and
// checks its stop token every period.
//
// Parked: while the app is Suspended or Idle, main calls SetParked(true)
// and the thread waits on an atomic doorbell (a sequence number with
// atomic::wait) until SetParked(false) or a stop rings it.
//
// SDL_UpdateGamepads pushes SDL events, so the sampler keeps to ADR-047's
// window: the App starts it after the first iterate and stops it before
// shutdown step 4, while WakeEventScope is open. It never touches World
// state, InputService, ImGui or the GPU.
//
// The poll and the wait are injectable (tests: a fake poll function on
// every platform, under TSan).

#ifndef SE_PLATFORM_INPUT_SAMPLER_H_
#define SE_PLATFORM_INPUT_SAMPLER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

#include "se_core/error.h"
#include "se_os/thread.h"

namespace se {

inline constexpr std::uint32_t kSamplerHzOnAc = 1000;
inline constexpr std::uint32_t kSamplerHzOnBattery = 250;
// While no gamepad is open: the 1 kHz updates stamp nothing then, and they
// cost main's VRR deadline about 0.2 ms at p99 on the development machine
// (ADR-090).
inline constexpr std::uint32_t kSamplerHzIdle = 20;

struct InputSamplerConfig {
  std::uint32_t rate_hz = kSamplerHzOnAc;
  bool start_parked = false;
  // Defaults: SDL_UpdateGamepads and SDL_DelayNS. Called on the sampler
  // thread only.
  std::function<void()> poll;
  std::function<void(std::uint64_t ns)> delay;
};

struct InputSamplerStats {
  std::uint64_t polls = 0;
  // Times the thread parked, and woke from a park.
  std::uint64_t parks = 0;
  std::uint64_t unparks = 0;
  std::uint32_t rate_hz = 0;
  // The thread is waiting on its doorbell now.
  bool parked = false;
  // Time spent polling (in total and the longest poll) and waiting.
  std::uint64_t poll_ns = 0;
  std::uint64_t poll_ns_max = 0;
  std::uint64_t wait_ns = 0;
};

class InputSampler {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<InputSampler>, Error>
  Start(InputSamplerConfig config);

  InputSampler(const InputSampler&) = delete;
  InputSampler& operator=(const InputSampler&) = delete;
  // Stop().
  ~InputSampler();

  // The polling rate (main; clamped to 1..2000 Hz).
  void SetRate(std::uint32_t hz) noexcept;
  // Parks or unparks the thread (main; never blocks).
  void SetParked(bool parked) noexcept;
  // Requests the stop, rings the doorbell and joins: within one period.
  // Idempotent.
  void Stop() noexcept;
  [[nodiscard]] bool Running() const noexcept { return thread_.Joinable(); }
  // Any thread.
  [[nodiscard]] InputSamplerStats Stats() const noexcept;

  struct State;

 private:
  explicit InputSampler(std::unique_ptr<State> state) noexcept;

  std::unique_ptr<State> state_;
  Thread thread_;
};

// The polling rate for the power source.
[[nodiscard]] constexpr std::uint32_t SamplerRateFor(bool on_battery) noexcept {
  return on_battery ? kSamplerHzOnBattery : kSamplerHzOnAc;
}

// The polling rate for the power source and the gamepads the App has open.
[[nodiscard]] constexpr std::uint32_t SamplerRateFor(
    bool on_battery, std::size_t open_gamepads) noexcept {
  return open_gamepads == 0 ? kSamplerHzIdle : SamplerRateFor(on_battery);
}

// Reads SDL_GetPowerInfo at most every |interval_ns| (main): Update returns
// the new rate when the power source changed (and on the first read).
class PowerWatch {
 public:
  static constexpr std::uint64_t kIntervalNs = 5'000'000'000;

  // The battery query; default: SDL_GetPowerInfo reports SDL_POWERSTATE_
  // ON_BATTERY.
  explicit PowerWatch(std::function<bool()> on_battery = {});

  [[nodiscard]] std::optional<std::uint32_t> Update(std::uint64_t now_ns);
  [[nodiscard]] bool OnBattery() const noexcept { return on_battery_; }

 private:
  std::function<bool()> query_;
  std::uint64_t next_ns_ = 0;
  bool known_ = false;
  bool on_battery_ = false;
};

// SDL_GetPowerInfo: true while the machine runs on its battery.
[[nodiscard]] bool OnBatteryPower();

}  // namespace se

#endif  // SE_PLATFORM_INPUT_SAMPLER_H_
