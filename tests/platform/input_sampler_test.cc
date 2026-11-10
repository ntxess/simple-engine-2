// Copyright 2026 The simple-engine-2 Authors.
//
// The input sampler's thread with a fake poll function (label unit,
// everywhere, TSan in WSL): it polls at its rate, parks on its doorbell and
// polls nothing while parked, wakes when unparked, stops promptly whether
// running or parked, and takes a new rate; PowerWatch picks 1000 or 250 Hz
// from the power source every 5 s. The real SDL_UpdateGamepads path is
// gamepad_desktop_test.cc.

#include "se_platform/input_sampler.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "se_core/thread_role.h"
#include "se_os/thread.h"

namespace se {
namespace {

using Clock = std::chrono::steady_clock;

// Waits up to 5 s for |done|.
template <class F>
bool WaitFor(const F& done) {
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  while (!done()) {
    if (Clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

struct Fake {
  std::atomic<std::uint64_t> polls{0};
  std::atomic<std::uint64_t> max_delay{0};
};

InputSamplerConfig FakeConfig(Fake& fake) {
  InputSamplerConfig config;
  config.poll = [&fake] { fake.polls.fetch_add(1); };
  // Returns at once: the sampler then asks for whole periods (its grid
  // starts over from each poll and the timer is never late).
  config.delay = [&fake](std::uint64_t ns) {
    if (ns > fake.max_delay.load()) fake.max_delay.store(ns);
    std::this_thread::yield();
  };
  return config;
}

TEST_CASE("The sampler polls every period at its rate", "[input_sampler]") {
  Fake fake;
  auto sampler = InputSampler::Start(FakeConfig(fake));
  REQUIRE(sampler);
  REQUIRE((*sampler)->Running());
  REQUIRE(WaitFor([&fake] { return fake.polls.load() >= 20; }));
  // No wait is longer than a period.
  REQUIRE(fake.max_delay.load() > 0);
  REQUIRE(fake.max_delay.load() <= 1'000'000);
  (*sampler)->SetRate(kSamplerHzOnBattery);
  REQUIRE(WaitFor([&] { return fake.max_delay.load() > 1'000'000; }));
  REQUIRE(fake.max_delay.load() <= 4'000'000);
  REQUIRE((*sampler)->Stats().rate_hz == 250);
  REQUIRE((*sampler)->Stats().polls >= 20);
  (*sampler)->Stop();
  REQUIRE_FALSE((*sampler)->Running());
}

TEST_CASE("A parked sampler polls nothing until it is unparked",
          "[input_sampler]") {
  Fake fake;
  auto sampler = InputSampler::Start(FakeConfig(fake));
  REQUIRE(sampler);
  REQUIRE(WaitFor([&fake] { return fake.polls.load() >= 5; }));
  (*sampler)->SetParked(true);
  REQUIRE(WaitFor([&] { return (*sampler)->Stats().parked; }));
  const std::uint64_t parked_at = fake.polls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  REQUIRE(fake.polls.load() == parked_at);
  REQUIRE((*sampler)->Stats().parks == 1);
  // Parking twice is one park.
  (*sampler)->SetParked(true);
  (*sampler)->SetParked(false);
  REQUIRE(WaitFor([&] { return fake.polls.load() > parked_at + 3; }));
  REQUIRE_FALSE((*sampler)->Stats().parked);
  REQUIRE((*sampler)->Stats().unparks >= 1);
}

TEST_CASE("Stop wakes a parked sampler and joins", "[input_sampler]") {
  Fake fake;
  InputSamplerConfig config = FakeConfig(fake);
  config.start_parked = true;
  auto sampler = InputSampler::Start(std::move(config));
  REQUIRE(sampler);
  REQUIRE(WaitFor([&] { return (*sampler)->Stats().parked; }));
  REQUIRE(fake.polls.load() == 0);
  const auto start = Clock::now();
  (*sampler)->Stop();
  const auto took = Clock::now() - start;
  REQUIRE_FALSE((*sampler)->Running());
  REQUIRE(took < std::chrono::seconds(1));
  // Stopping twice, and destroying a stopped sampler, are fine.
  (*sampler)->Stop();
}

TEST_CASE("Stop joins a running sampler within its period", "[input_sampler]") {
  Fake fake;
  InputSamplerConfig config;
  config.poll = [&fake] { fake.polls.fetch_add(1); };
  // The real wait at 1 kHz.
  config.delay = [](std::uint64_t ns) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
  };
  auto sampler = InputSampler::Start(std::move(config));
  REQUIRE(sampler);
  REQUIRE(WaitFor([&fake] { return fake.polls.load() >= 10; }));
  const auto start = Clock::now();
  sampler->reset();
  REQUIRE(Clock::now() - start < std::chrono::milliseconds(500));
}

TEST_CASE("The sampler runs as the input-sampler role", "[input_sampler]") {
  std::atomic<int> role{-1};
  InputSamplerConfig config;
  config.poll = [&role] { role.store(static_cast<int>(CurrentRole())); };
  config.delay = [](std::uint64_t) {
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  };
  auto sampler = InputSampler::Start(std::move(config));
  REQUIRE(sampler);
  REQUIRE(WaitFor([&role] { return role.load() >= 0; }));
  REQUIRE(role.load() == static_cast<int>(ThreadRole::kInputSampler));
}

TEST_CASE("PowerWatch picks the rate from the power source every 5 s",
          "[input_sampler]") {
  bool battery = false;
  int queries = 0;
  PowerWatch watch([&battery, &queries] {
    ++queries;
    return battery;
  });
  const std::uint64_t s = 1'000'000'000;
  REQUIRE(watch.Update(10 * s) == std::optional<std::uint32_t>(1000));
  REQUIRE(queries == 1);
  battery = true;
  // Not read again before 5 s.
  REQUIRE_FALSE(watch.Update(12 * s));
  REQUIRE(queries == 1);
  REQUIRE(watch.Update(15 * s) == std::optional<std::uint32_t>(250));
  REQUIRE(watch.OnBattery());
  // Unchanged: nothing to publish.
  REQUIRE_FALSE(watch.Update(21 * s));
  REQUIRE(queries == 3);
  REQUIRE(SamplerRateFor(false) == kSamplerHzOnAc);
  REQUIRE(SamplerRateFor(true) == kSamplerHzOnBattery);
}

}  // namespace
}  // namespace se
