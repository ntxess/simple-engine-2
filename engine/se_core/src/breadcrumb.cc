// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/breadcrumb.h"

#include <atomic>
#include <cstdint>

#include "se_core/dump_stream.h"
#include "se_core/thread_role.h"

namespace se {

void Breadcrumb::SetField(std::uint64_t value, std::uint64_t mask,
                          int shift) noexcept {
  const std::atomic_ref<std::uint64_t> word(Word());
  const std::uint64_t old = word.load(std::memory_order_relaxed);
  const std::uint64_t cleared = old & ~(mask << shift);
  word.store(cleared | ((value & mask) << shift), std::memory_order_relaxed);
}

std::uint64_t Breadcrumb::Packed() const noexcept {
  return AtomicLoad(Word(), std::memory_order_relaxed);
}

void Breadcrumb::Bind(std::uint64_t* word) noexcept {
  const std::uint64_t value = Packed();
  word_ = word;
  std::atomic_ref<std::uint64_t>(Word()).store(value,
                                               std::memory_order_relaxed);
}

void Breadcrumb::SetRole(ThreadRole role) noexcept {
  SetField(static_cast<std::uint64_t>(role), kBreadcrumbRoleMask,
           kBreadcrumbRoleShift);
}

void Breadcrumb::SetPhase(std::uint8_t phase) noexcept {
  SetField(phase, kBreadcrumbPhaseMask, kBreadcrumbPhaseShift);
}

void Breadcrumb::SetSystem(std::uint16_t system) noexcept {
  SetField(system, kBreadcrumbSystemMask, kBreadcrumbSystemShift);
}

void Breadcrumb::SetChunk(std::uint32_t chunk) noexcept {
  SetField(chunk, kBreadcrumbChunkMask, kBreadcrumbChunkShift);
}

void Breadcrumb::SetTick(std::uint64_t tick) noexcept {
  SetField(tick, kBreadcrumbTickMask, 0);
}

Breadcrumb& ThreadBreadcrumb() noexcept {
  thread_local Breadcrumb breadcrumb;
  return breadcrumb;
}

}  // namespace se
