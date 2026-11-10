// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/driver_wait.h"

#include <atomic>

namespace se {
namespace {

// Written by main only (release stores), read by the watchdog.
constinit std::atomic<bool> g_in_driver{false};

}  // namespace

const std::atomic<bool>& DriverWaitFlag() noexcept { return g_in_driver; }

bool InDriverWait() noexcept {
  return g_in_driver.load(std::memory_order_acquire);
}

DriverWaitScope::DriverWaitScope() noexcept
    : previous_(g_in_driver.load(std::memory_order_relaxed)) {
  g_in_driver.store(true, std::memory_order_release);
}

DriverWaitScope::~DriverWaitScope() {
  g_in_driver.store(previous_, std::memory_order_release);
}

}  // namespace se
