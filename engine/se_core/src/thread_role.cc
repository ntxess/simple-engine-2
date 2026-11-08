// Copyright 2026 The simple-engine-2 Authors.

#include "se_core/thread_role.h"

#include <atomic>
#include <string_view>
#include <thread>

#include "se_core/assert.h"
#include "se_core/breadcrumb.h"

namespace se {

MainThreadRole g_main;
WorldOwnerRole g_sim_owner;

namespace {

std::atomic<std::thread::id> g_main_thread_id{};
thread_local ThreadRole t_role = ThreadRole::kLibrary;
thread_local int t_inline_owner_depth = 0;

}  // namespace

// The role capabilities are static tokens with nothing to lock, so the
// functions that acquire and release them are excluded from the analysis.
void RegisterMainThread() noexcept SE_NO_THREAD_SAFETY_ANALYSIS {
  g_main_thread_id.store(std::this_thread::get_id(), std::memory_order_release);
  SetCurrentRole(ThreadRole::kMain);
}

void UnregisterMainThread() noexcept SE_NO_THREAD_SAFETY_ANALYSIS {
  g_main_thread_id.store(std::thread::id{}, std::memory_order_release);
}

bool IsMainThread() noexcept {
  return g_main_thread_id.load(std::memory_order_acquire) ==
         std::this_thread::get_id();
}

ThreadRole CurrentRole() noexcept { return t_role; }

void SetCurrentRole(ThreadRole role) noexcept {
  t_role = role;
  ThreadBreadcrumb().SetRole(role);
}

std::string_view RoleName(ThreadRole role) noexcept {
  switch (role) {
    case ThreadRole::kLibrary:
      return "lib";
    case ThreadRole::kMain:
      return "main";
    case ThreadRole::kSim:
      return "sim";
    case ThreadRole::kForkWorker:
      return "fork";
    case ThreadRole::kBackground:
      return "bg";
    case ThreadRole::kLogWorker:
      return "log";
    case ThreadRole::kWatchdog:
      return "watchdog";
    case ThreadRole::kInputSampler:
      return "input";
    case ThreadRole::kVblankSampler:
      return "vblank";
  }
  return "?";
}

// The capability is a static token, so there is nothing to lock.
InlineOwnerScope::InlineOwnerScope() noexcept SE_NO_THREAD_SAFETY_ANALYSIS {
  ++t_inline_owner_depth;
}

InlineOwnerScope::~InlineOwnerScope() SE_NO_THREAD_SAFETY_ANALYSIS {
  --t_inline_owner_depth;
}

bool InlineOwnerScope::HeldOnThisThread() noexcept {
  return t_inline_owner_depth > 0;
}

void AssertMainThread() noexcept {
  SE_ASSERT_MSG(IsMainThread(), "not on the main thread");
}

}  // namespace se
