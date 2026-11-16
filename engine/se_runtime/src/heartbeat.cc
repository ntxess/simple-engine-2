// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/heartbeat.h"

#include <SDL3/SDL_timer.h>

#include <atomic>
#include <cstdint>
#include <string_view>

namespace se {

HeartbeatScope::HeartbeatScope(std::atomic<bool>& flag, std::string_view what,
                               LongWorkStats* stats) noexcept
    : flag_(&flag),
      what_(what),
      stats_(stats),
      start_ns_(SDL_GetTicksNS()),
      previous_(flag.load(std::memory_order_relaxed)) {
  flag_->store(true, std::memory_order_release);
}

HeartbeatScope::~HeartbeatScope() {
  flag_->store(previous_, std::memory_order_release);
  if (stats_ == nullptr) return;
  const std::uint64_t took = SDL_GetTicksNS() - start_ns_;
  ++stats_->scopes;
  if (took >= stats_->longest_ns) {
    stats_->longest_ns = took;
    stats_->longest = what_;
  }
}

}  // namespace se
